//===--- SILGenConstructor.cpp - SILGen for constructors ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGenFunction.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "Scope.h"
#include "swift/AST/AST.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Basic/Defer.h"

using namespace swift;
using namespace Lowering;

static SILValue emitConstructorMetatypeArg(SILGenFunction &gen,
                                           ValueDecl *ctor) {
  // In addition to the declared arguments, the constructor implicitly takes
  // the metatype as its first argument, like a static function.
  Type metatype = ctor->getType()->castTo<AnyFunctionType>()->getInput();
  auto &AC = gen.getASTContext();
  auto VD = new (AC) ParamDecl(/*IsLet*/ true, SourceLoc(),
                               AC.getIdentifier("$metatype"), SourceLoc(),
                               AC.getIdentifier("$metatype"), metatype,
                               ctor->getDeclContext());
  gen.AllocatorMetatype = new (gen.F.getModule()) SILArgument(gen.F.begin(),
                                              gen.getLoweredType(metatype), VD);
  return gen.AllocatorMetatype;
}

static RValue emitImplicitValueConstructorArg(SILGenFunction &gen,
                                              SILLocation loc,
                                              CanType ty,
                                              DeclContext *DC) {
  // Restructure tuple arguments.
  if (CanTupleType tupleTy = dyn_cast<TupleType>(ty)) {
    RValue tuple(ty);
    for (auto fieldType : tupleTy.getElementTypes())
      tuple.addElement(emitImplicitValueConstructorArg(gen, loc, fieldType, DC));

    return tuple;
  } else {
    auto &AC = gen.getASTContext();
    auto VD = new (AC) ParamDecl(/*IsLet*/ true, SourceLoc(),
                                 AC.getIdentifier("$implicit_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$implicit_value"), ty, DC);
    SILValue arg = new (gen.F.getModule()) SILArgument(gen.F.begin(),
                                                       gen.getLoweredType(ty),
                                                       VD);
    return RValue(gen, loc, ty, gen.emitManagedRValueWithCleanup(arg));
  }
}

static void emitImplicitValueConstructor(SILGenFunction &gen,
                                         ConstructorDecl *ctor) {
  RegularLocation Loc(ctor);
  Loc.markAutoGenerated();
  // FIXME: Handle 'self' along with the other arguments.
  auto *TP = cast<TuplePattern>(ctor->getBodyParamPatterns()[1]);
  auto selfTyCan = ctor->getImplicitSelfDecl()->getType()->getInOutObjectType();
  SILType selfTy = gen.getLoweredType(selfTyCan);

  // Emit the indirect return argument, if any.
  SILValue resultSlot;
  if (selfTy.isAddressOnly(gen.SGM.M)) {
    auto &AC = gen.getASTContext();
    auto VD = new (AC) ParamDecl(/*IsLet*/ false, SourceLoc(),
                                 AC.getIdentifier("$return_value"),
                                 SourceLoc(),
                                 AC.getIdentifier("$return_value"), selfTyCan,
                                 ctor);
    resultSlot = new (gen.F.getModule()) SILArgument(gen.F.begin(), selfTy, VD);
  }

  // Emit the elementwise arguments.
  SmallVector<RValue, 4> elements;
  for (size_t i = 0, size = TP->getNumElements(); i < size; ++i) {
    auto *P = cast<TypedPattern>(TP->getElement(i).getPattern());

    elements.push_back(
      emitImplicitValueConstructorArg(gen, Loc,
                                      P->getType()->getCanonicalType(), ctor));
  }

  emitConstructorMetatypeArg(gen, ctor);

  auto *decl = selfTy.getStructOrBoundGenericStruct();
  assert(decl && "not a struct?!");

  // If we have an indirect return slot, initialize it in-place.
  if (resultSlot) {

    auto elti = elements.begin(), eltEnd = elements.end();
    for (VarDecl *field : decl->getStoredProperties()) {
      auto fieldTy = selfTy.getFieldType(field, gen.SGM.M);
      auto &fieldTL = gen.getTypeLowering(fieldTy);
      SILValue slot = gen.B.createStructElementAddr(Loc, resultSlot, field,
                                                    fieldTL.getLoweredType().getAddressType());
      InitializationPtr init(new KnownAddressInitialization(slot));

      // An initialized 'let' property has a single value specified by the
      // initializer - it doesn't come from an argument.
      if (!field->isStatic() && field->isLet() &&
          field->getParentInitializer()) {
        assert(field->getType()->isEqual(field->getParentInitializer()
                                         ->getType()) && "Checked by sema");

        // Cleanup after this initialization.
        FullExpr scope(gen.Cleanups, field->getParentPatternBinding());
        gen.emitRValue(field->getParentInitializer())
          .forwardInto(gen, init.get(), Loc);
        continue;
      }

      assert(elti != eltEnd && "number of args does not match number of fields");
      (void)eltEnd;
      std::move(*elti).forwardInto(gen, init.get(), Loc);
      ++elti;
    }
    gen.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                       gen.emitEmptyTuple(Loc));
    return;
  }

  // Otherwise, build a struct value directly from the elements.
  SmallVector<SILValue, 4> eltValues;

  auto elti = elements.begin(), eltEnd = elements.end();
  for (VarDecl *field : decl->getStoredProperties()) {
    auto fieldTy = selfTy.getFieldType(field, gen.SGM.M);
    SILValue v;

    // An initialized 'let' property has a single value specified by the
    // initializer - it doesn't come from an argument.
    if (!field->isStatic() && field->isLet() && field->getParentInitializer()) {
      // Cleanup after this initialization.
      FullExpr scope(gen.Cleanups, field->getParentPatternBinding());
      v = gen.emitRValue(field->getParentInitializer())
             .forwardAsSingleStorageValue(gen, fieldTy, Loc);
    } else {
      assert(elti != eltEnd && "number of args does not match number of fields");
      (void)eltEnd;
      v = std::move(*elti).forwardAsSingleStorageValue(gen, fieldTy, Loc);
      ++elti;
    }

    eltValues.push_back(v);
  }

  SILValue selfValue = gen.B.createStruct(Loc, selfTy, eltValues);
  gen.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                     selfValue);
  return;
}

void SILGenFunction::emitValueConstructor(ConstructorDecl *ctor) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ctor);

  if (ctor->isMemberwiseInitializer())
    return emitImplicitValueConstructor(*this, ctor);

  // True if this constructor delegates to a peer constructor with self.init().
  bool isDelegating = ctor->getDelegatingOrChainedInitKind(nullptr) ==
    ConstructorDecl::BodyInitKind::Delegating;

  // Get the 'self' decl and type.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  auto &lowering = getTypeLowering(selfDecl->getType()->getInOutObjectType());
  SILType selfTy = lowering.getLoweredType();
  (void)selfTy;
  assert(!selfTy.getClassOrBoundGenericClass()
         && "can't emit a class ctor here");

  
  // Allocate the local variable for 'self'.
  emitLocalVariableWithCleanup(selfDecl, false)->finishInitialization(*this);
  
  // Mark self as being uninitialized so that DI knows where it is and how to
  // check for it.
  SILValue selfLV;
  {
    auto &SelfVarLoc = VarLocs[selfDecl];
    auto MUIKind =  isDelegating ? MarkUninitializedInst::DelegatingSelf
                                 : MarkUninitializedInst::RootSelf;
    selfLV = B.createMarkUninitialized(selfDecl, SelfVarLoc.value, MUIKind);
    SelfVarLoc.value = selfLV;
  }
  
  // Emit the prolog.
  emitProlog(ctor->getBodyParamPatterns()[1], ctor->getResultType(), ctor);
  emitConstructorMetatypeArg(*this, ctor);

  // Create a basic block to jump to for the implicit 'self' return.
  // We won't emit this until after we've emitted the body.
  // The epilog takes a void return because the return of 'self' is implicit.
  prepareEpilog(Type(), ctor->isBodyThrowing(), CleanupLocation(ctor));

  // If the constructor can fail, set up an alternative epilog for constructor
  // failure.
  SILBasicBlock *failureExitBB = nullptr;
  SILArgument *failureExitArg = nullptr;
  auto &resultLowering = getTypeLowering(ctor->getResultType());

  if (ctor->getFailability() != OTK_None) {
    SILBasicBlock *failureBB = createBasicBlock(FunctionSection::Postmatter);

    // On failure, we'll clean up everything (except self, which should have
    // been cleaned up before jumping here) and return nil instead.
    SavedInsertionPoint savedIP(*this, failureBB, FunctionSection::Postmatter);
    failureExitBB = createBasicBlock();
    Cleanups.emitCleanupsForReturn(ctor);
    // Return nil.
    if (lowering.isAddressOnly()) {
      // Inject 'nil' into the indirect return.
      B.createInjectEnumAddr(ctor, IndirectReturnAddress,
                   getASTContext().getOptionalNoneDecl(ctor->getFailability()));
      B.createBranch(ctor, failureExitBB);

      B.setInsertionPoint(failureExitBB);
      B.createReturn(ctor, emitEmptyTuple(ctor));
    } else {
      // Pass 'nil' as the return value to the exit BB.
      failureExitArg = new (F.getModule())
        SILArgument(failureExitBB, resultLowering.getLoweredType());
      SILValue nilResult = B.createEnum(ctor, {},
                    getASTContext().getOptionalNoneDecl(ctor->getFailability()),
                    resultLowering.getLoweredType());
      B.createBranch(ctor, failureExitBB, nilResult);

      B.setInsertionPoint(failureExitBB);
      B.createReturn(ctor, failureExitArg);
    }

    FailDest = JumpDest(failureBB, Cleanups.getCleanupsDepth(), ctor);
  }

  // If this is not a delegating constructor, emit member initializers.
  if (!isDelegating) {
    auto nominal = ctor->getDeclContext()->getDeclaredTypeInContext()
                     ->getNominalOrBoundGenericNominal();
    emitMemberInitializers(selfDecl, nominal);
  }

  emitProfilerIncrement(ctor->getBody());
  // Emit the constructor body.
  emitStmt(ctor->getBody());

  
  // Build a custom epilog block, since the AST representation of the
  // constructor decl (which has no self in the return type) doesn't match the
  // SIL representation.
  SILValue selfValue;
  {
    SavedInsertionPoint savedIP(*this, ReturnDest.getBlock());
    assert(B.getInsertionBB()->empty() && "Epilog already set up?");
    
    auto cleanupLoc = CleanupLocation::get(ctor);
    
    if (!lowering.isAddressOnly()) {
      // Otherwise, load and return the final 'self' value.
      selfValue = B.createLoad(cleanupLoc, selfLV);
      
      // Emit a retain of the loaded value, since we return it +1.
      lowering.emitRetainValue(B, cleanupLoc, selfValue);
      
      // Inject the self value into an optional if the constructor is failable.
      if (ctor->getFailability() != OTK_None) {
        selfValue = B.createEnum(ctor, selfValue,
                                 getASTContext().getOptionalSomeDecl(ctor->getFailability()),
                                 getLoweredLoadableType(ctor->getResultType()));
      }
    } else {
      // If 'self' is address-only, copy 'self' into the indirect return slot.
      assert(IndirectReturnAddress &&
             "no indirect return for address-only ctor?!");
      
      // Get the address to which to store the result.
      SILValue returnAddress;
      switch (ctor->getFailability()) {
      // For non-failable initializers, store to the return address directly.
      case OTK_None:
        returnAddress = IndirectReturnAddress;
        break;
      // If this is a failable initializer, project out the payload.
      case OTK_Optional:
      case OTK_ImplicitlyUnwrappedOptional:
        returnAddress = B.createInitEnumDataAddr(ctor, IndirectReturnAddress,
                 getASTContext().getOptionalSomeDecl(ctor->getFailability()),
                                                 selfLV.getType());
        break;
      }
      
      // We have to do a non-take copy because someone else may be using the
      // box (e.g. someone could have closed over it).
      B.createCopyAddr(cleanupLoc, selfLV, returnAddress,
                       IsNotTake, IsInitialization);
      
      // Inject the enum tag if the result is optional because of failability.
      if (ctor->getFailability() != OTK_None) {
        // Inject the 'Some' tag.
        B.createInjectEnumAddr(ctor, IndirectReturnAddress,
                               getASTContext().getOptionalSomeDecl(ctor->getFailability()));
      }
    }
  }
  
  // Finally, emit the epilog and post-matter.
  auto returnLoc = emitEpilog(ctor, /*UsesCustomEpilog*/true);

  // Finish off the epilog by returning.  If this is a failable ctor, then we
  // actually jump to the failure epilog to keep the invariant that there is
  // only one SIL return instruction per SIL function.
  if (B.hasValidInsertionPoint()) {
    if (!failureExitBB) {
      if (!selfValue)
        selfValue = emitEmptyTuple(ctor);
      
      B.createReturn(returnLoc, selfValue);
    } else {
      if (selfValue)
        B.createBranch(returnLoc, failureExitBB, selfValue);
      else
        B.createBranch(returnLoc, failureExitBB);
    }
  }
}

static void boxIndirectEnumPayload(SILGenFunction &gen,
                                   ManagedValue &payload,
                                   SILLocation loc,
                                   EnumElementDecl *element) {
  // If the payload is indirect, we'll need to box it.
  if (payload && (element->isIndirect() ||
                  element->getParentEnum()->isIndirect())) {
    auto box = gen.B.createAllocBox(loc, payload.getType());
    payload.forwardInto(gen, loc, box->getAddressResult());
    payload = gen.emitManagedRValueWithCleanup(box);
  }
}

static void emitAddressOnlyEnumConstructor(SILGenFunction &gen,
                                           SILType enumTy,
                                           EnumElementDecl *element) {
  RegularLocation Loc(element);
  CleanupLocation CleanupLoc(element);
  Loc.markAutoGenerated();

  // Emit the indirect return slot.
  auto &AC = gen.getASTContext();
  auto VD = new (AC) ParamDecl(/*IsLet*/ false, SourceLoc(),
                               AC.getIdentifier("$return_value"),
                               SourceLoc(),
                               AC.getIdentifier("$return_value"),
                               enumTy.getSwiftType(),
                               element->getDeclContext());
  SILValue resultSlot
    = new (gen.F.getModule()) SILArgument(gen.F.begin(), enumTy, VD);

  Scope scope(gen.Cleanups, CleanupLoc);

  // Emit the exploded constructor argument.
  ManagedValue argValue;
  if (element->hasArgumentType()) {
    RValue arg = emitImplicitValueConstructorArg
      (gen, Loc, element->getArgumentType()->getCanonicalType(),
       element->getDeclContext());
    argValue = std::move(arg).getAsSingleValue(gen, Loc);
  }
  emitConstructorMetatypeArg(gen, element);

  boxIndirectEnumPayload(gen, argValue, Loc, element);

  // Store the data, if any.
  if (argValue) {
    SILValue resultData = gen.B.createInitEnumDataAddr(element, resultSlot,
      element, argValue.getType().getAddressType());
    argValue.forwardInto(gen, element, resultData);
  }

  // Apply the tag.
  gen.B.createInjectEnumAddr(Loc, resultSlot, element);
  scope.pop();
  gen.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                     gen.emitEmptyTuple(element));
}

static void emitLoadableEnumConstructor(SILGenFunction &gen, SILType enumTy,
                                        EnumElementDecl *element) {
  RegularLocation Loc(element);
  CleanupLocation CleanupLoc(element);
  Loc.markAutoGenerated();

  Scope scope(gen.Cleanups, CleanupLoc);

  // Emit the exploded constructor argument.
  ManagedValue payload;
  if (element->hasArgumentType()) {
    RValue arg = emitImplicitValueConstructorArg
      (gen, Loc,
       element->getArgumentType()->getCanonicalType(),
       element->getDeclContext());
    payload = std::move(arg).getAsSingleValue(gen, Loc);
  }

  emitConstructorMetatypeArg(gen, element);

  boxIndirectEnumPayload(gen, payload, Loc, element);

  // Create and return the enum value.
  SILValue argValue;
  if (payload)
    argValue = payload.forward(gen);
  SILValue result = gen.B.createEnum(Loc, argValue, element, enumTy);
  scope.pop();
  gen.B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc), result);
}

void SILGenFunction::emitEnumConstructor(EnumElementDecl *element) {
  Type enumTy = element->getType()->getAs<AnyFunctionType>()->getResult();
  if (element->hasArgumentType())
    enumTy = enumTy->getAs<AnyFunctionType>()->getResult();
  auto &enumTI = getTypeLowering(enumTy);

  if (enumTI.isAddressOnly()) {
    return emitAddressOnlyEnumConstructor(*this, enumTI.getLoweredType(),
                                           element);
  }
  return emitLoadableEnumConstructor(*this, enumTI.getLoweredType(),
                                      element);
}

bool Lowering::usesObjCAllocator(ClassDecl *theClass) {
  while (true) {
    // If the root class was implemented in Objective-C, use Objective-C's
    // allocation methods because they may have been overridden.
    if (!theClass->hasSuperclass())
      return theClass->hasClangNode();

    theClass = theClass->getSuperclass()->getClassOrBoundGenericClass();
  }
}

void SILGenFunction::emitClassConstructorAllocator(ConstructorDecl *ctor) {
  assert(!ctor->isFactoryInit() && "factories should not be emitted here");

  // Emit the prolog. Since we're just going to forward our args directly
  // to the initializer, don't allocate local variables for them.
  RegularLocation Loc(ctor);
  Loc.markAutoGenerated();

  // Forward the constructor arguments.
  // FIXME: Handle 'self' along with the other body patterns.
  SmallVector<SILValue, 8> args;
  bindParametersForForwarding(ctor->getBodyParamPatterns()[1], args);

  SILValue selfMetaValue = emitConstructorMetatypeArg(*this, ctor);

  // Allocate the "self" value.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  SILType selfTy = getLoweredType(selfDecl->getType());
  assert(selfTy.hasReferenceSemantics() &&
         "can't emit a value type ctor here");

  // Use alloc_ref to allocate the object.
  // TODO: allow custom allocation?
  // FIXME: should have a cleanup in case of exception
  auto selfTypeContext = ctor->getDeclContext()->getDeclaredTypeInContext();
  auto selfClassDecl =
    cast<ClassDecl>(selfTypeContext->getNominalOrBoundGenericNominal());

  SILValue selfValue;

  // Allocate the 'self' value.
  bool useObjCAllocation = usesObjCAllocator(selfClassDecl);

  if (ctor->isConvenienceInit() || ctor->hasClangNode()) {
    // For a convenience initializer or an initializer synthesized
    // for an Objective-C class, allocate using the metatype.
    SILValue allocArg = selfMetaValue;

    // When using Objective-C allocation, convert the metatype
    // argument to an Objective-C metatype.
    if (useObjCAllocation) {
      auto metaTy = allocArg.getType().castTo<MetatypeType>();
      metaTy = CanMetatypeType::get(metaTy.getInstanceType(),
                                    MetatypeRepresentation::ObjC);
      allocArg = B.createThickToObjCMetatype(Loc, allocArg,
                                             getLoweredType(metaTy));
    }

    selfValue = B.createAllocRefDynamic(Loc, allocArg, selfTy,
                                        useObjCAllocation);
  } else {
    // For a designated initializer, we know that the static type being
    // allocated is the type of the class that defines the designated
    // initializer.
    selfValue = B.createAllocRef(Loc, selfTy, useObjCAllocation, false);
  }
  args.push_back(selfValue);

  // Call the initializer. Always use the Swift entry point, which will be a
  // bridging thunk if we're calling ObjC.
  SILDeclRef initConstant =
    SILDeclRef(ctor,
               SILDeclRef::Kind::Initializer,
               SILDeclRef::ConstructAtBestResilienceExpansion,
               SILDeclRef::ConstructAtNaturalUncurryLevel,
               /*isObjC=*/false);

  ManagedValue initVal;
  SILType initTy;

  ArrayRef<Substitution> subs;
  // Call the initializer.
  ArrayRef<Substitution> forwardingSubs;
  if (auto *genericParamList = ctor->getGenericParamsOfContext())
    forwardingSubs =
        genericParamList->getForwardingSubstitutions(getASTContext());
  std::tie(initVal, initTy, subs)
    = emitSiblingMethodRef(Loc, selfValue, initConstant, forwardingSubs);

  SILValue initedSelfValue = emitApplyWithRethrow(Loc, initVal.forward(*this),
                                                  initTy, subs, args);

  // Return the initialized 'self'.
  B.createReturn(ImplicitReturnLocation::getImplicitReturnLoc(Loc),
                 initedSelfValue);
}

void SILGenFunction::emitClassConstructorInitializer(ConstructorDecl *ctor) {
  MagicFunctionName = SILGenModule::getMagicFunctionName(ctor);

  assert(ctor->getBody() && "Class constructor without a body?");

  // True if this constructor delegates to a peer constructor with self.init().
  bool isDelegating = false;
  if (!ctor->hasStubImplementation()) {
    isDelegating = ctor->getDelegatingOrChainedInitKind(nullptr) ==
      ConstructorDecl::BodyInitKind::Delegating;
  }

  // FIXME: The (potentially partially initialized) value here would need to be
  // cleaned up on a constructor failure unwinding, if we were to support
  // failing before total initialization.

  // Set up the 'self' argument.  If this class has a superclass, we set up
  // self as a box.  This allows "self reassignment" to happen in super init
  // method chains, which is important for interoperating with Objective-C
  // classes.  We also use a box for delegating constructors, since the
  // delegated-to initializer may also replace self.
  //
  // TODO: If we could require Objective-C classes to have an attribute to get
  // this behavior, we could avoid runtime overhead here.
  VarDecl *selfDecl = ctor->getImplicitSelfDecl();
  auto selfTypeContext = ctor->getDeclContext()->getDeclaredTypeInContext();
  auto selfClassDecl =
    cast<ClassDecl>(selfTypeContext->getNominalOrBoundGenericNominal());
  bool NeedsBoxForSelf = isDelegating ||
    (selfClassDecl->hasSuperclass() && !ctor->hasStubImplementation());
  bool usesObjCAllocator = Lowering::usesObjCAllocator(selfClassDecl);

  // If needed, mark 'self' as uninitialized so that DI knows to
  // enforce its DI properties on stored properties.
  MarkUninitializedInst::Kind MUKind;

  if (isDelegating)
    MUKind = MarkUninitializedInst::DelegatingSelf;
  else if (selfClassDecl->requiresStoredPropertyInits() &&
           usesObjCAllocator) {
    // Stored properties will be initialized in a separate
    // .cxx_construct method called by the Objective-C runtime.
    assert(selfClassDecl->hasSuperclass() &&
           "Cannot use ObjC allocation without a superclass");
    MUKind = MarkUninitializedInst::DerivedSelfOnly;
  } else if (selfClassDecl->hasSuperclass())
    MUKind = MarkUninitializedInst::DerivedSelf;
  else
    MUKind = MarkUninitializedInst::RootSelf;

  if (NeedsBoxForSelf) {
    // Allocate the local variable for 'self'.
    emitLocalVariableWithCleanup(selfDecl, false)->finishInitialization(*this);

    auto &SelfVarLoc = VarLocs[selfDecl];
    SelfVarLoc.value = B.createMarkUninitialized(selfDecl,
                                                 SelfVarLoc.value, MUKind);
  }

  // Emit the prolog for the non-self arguments.
  // FIXME: Handle self along with the other body patterns.
  emitProlog(ctor->getBodyParamPatterns()[1],
             TupleType::getEmpty(F.getASTContext()), ctor);

  SILType selfTy = getLoweredLoadableType(selfDecl->getType());
  SILValue selfArg = new (SGM.M) SILArgument(F.begin(), selfTy, selfDecl);

  if (!NeedsBoxForSelf) {
    SILLocation PrologueLoc(selfDecl);
    PrologueLoc.markAsPrologue();
    B.createDebugValue(PrologueLoc, selfArg);
  }

  if (!ctor->hasStubImplementation()) {
    assert(selfTy.hasReferenceSemantics() &&
           "can't emit a value type ctor here");
    if (NeedsBoxForSelf) {
      SILLocation prologueLoc = RegularLocation(ctor);
      prologueLoc.markAsPrologue();
      B.createStore(prologueLoc, selfArg, VarLocs[selfDecl].value);
    } else {
      selfArg = B.createMarkUninitialized(selfDecl, selfArg, MUKind);
      VarLocs[selfDecl] = VarLoc::get(selfArg);
      enterDestroyCleanup(VarLocs[selfDecl].value);
    }
  }

  // Prepare the end of initializer location.
  SILLocation endOfInitLoc = RegularLocation(ctor);
  endOfInitLoc.pointToEnd();

  // Create a basic block to jump to for the implicit 'self' return.
  // We won't emit the block until after we've emitted the body.
  prepareEpilog(Type(), ctor->isBodyThrowing(),
                CleanupLocation::get(endOfInitLoc));

  // If the constructor can fail, set up an alternative epilog for constructor
  // failure.
  SILBasicBlock *failureExitBB = nullptr;
  SILArgument *failureExitArg = nullptr;
  auto &resultLowering = getTypeLowering(ctor->getResultType());

  if (ctor->getFailability() != OTK_None) {
    SILBasicBlock *failureBB = createBasicBlock(FunctionSection::Postmatter);

    RegularLocation loc(ctor);
    loc.markAutoGenerated();

    // On failure, we'll clean up everything and return nil instead.
    SavedInsertionPoint savedIP(*this, failureBB, FunctionSection::Postmatter);

    failureExitBB = createBasicBlock();
    failureExitArg = new (F.getModule())
      SILArgument(failureExitBB, resultLowering.getLoweredType());

    Cleanups.emitCleanupsForReturn(ctor);
    SILValue nilResult = B.createEnum(loc, {},
                    getASTContext().getOptionalNoneDecl(ctor->getFailability()),
                    resultLowering.getLoweredType());
    B.createBranch(loc, failureExitBB, nilResult);

    B.setInsertionPoint(failureExitBB);
    B.createReturn(loc, failureExitArg)->setDebugScope(F.getDebugScope());

    FailDest = JumpDest(failureBB, Cleanups.getCleanupsDepth(), ctor);
  }

  // Handle member initializers.
  if (isDelegating) {
    // A delegating initializer does not initialize instance
    // variables.
  } else if (ctor->hasStubImplementation()) {
    // Nor does a stub implementation.
  } else if (selfClassDecl->requiresStoredPropertyInits() &&
             usesObjCAllocator) {
    // When the class requires all stored properties to have initial
    // values and we're using Objective-C's allocation, stored
    // properties are initialized via the .cxx_construct method, which
    // will be called by the runtime.

    // Note that 'self' has been fully initialized at this point.
  } else {
    // Emit the member initializers.
    emitMemberInitializers(selfDecl, selfClassDecl);
  }

  emitProfilerIncrement(ctor->getBody());
  // Emit the constructor body.
  emitStmt(ctor->getBody());

  
  // Build a custom epilog block, since the AST representation of the
  // constructor decl (which has no self in the return type) doesn't match the
  // SIL representation.
  {
    SavedInsertionPoint savedIP(*this, ReturnDest.getBlock());
    assert(B.getInsertionBB()->empty() && "Epilog already set up?");
    auto cleanupLoc = CleanupLocation(ctor);

    // If we're using a box for self, reload the value at the end of the init
    // method.
    if (NeedsBoxForSelf) {
      // Emit the call to super.init() right before exiting from the initializer.
      if (Expr *SI = ctor->getSuperInitCall())
        emitRValue(SI);
      
      selfArg = B.createLoad(cleanupLoc, VarLocs[selfDecl].value);
    }
    
    // We have to do a retain because we are returning the pointer +1.
    B.emitRetainValueOperation(cleanupLoc, selfArg);

    // Inject the self value into an optional if the constructor is failable.
    if (ctor->getFailability() != OTK_None) {
      RegularLocation loc(ctor);
      loc.markAutoGenerated();
      selfArg = B.createEnum(loc, selfArg,
                 getASTContext().getOptionalSomeDecl(ctor->getFailability()),
                             getLoweredLoadableType(ctor->getResultType()));
    }
  }
  
  // Emit the epilog and post-matter.
  auto returnLoc = emitEpilog(ctor, /*UsesCustomEpilog*/true);

  // Finish off the epilog by returning.  If this is a failable ctor, then we
  // actually jump to the failure epilog to keep the invariant that there is
  // only one SIL return instruction per SIL function.
  if (B.hasValidInsertionPoint()) {
    if (failureExitBB)
      B.createBranch(returnLoc, failureExitBB, selfArg);
    else
      B.createReturn(returnLoc, selfArg)->setDebugScope(F.getDebugScope());
  }
}


/// Emit a member initialization for the members described in the
/// given pattern from the given source value.
static void emitMemberInit(SILGenFunction &SGF, VarDecl *selfDecl,
                           Pattern *pattern, RValue &&src) {
  switch (pattern->getKind()) {
  case PatternKind::Paren:
    return emitMemberInit(SGF, selfDecl,
                          cast<ParenPattern>(pattern)->getSubPattern(),
                          std::move(src));

  case PatternKind::Tuple: {
    auto tuple = cast<TuplePattern>(pattern);
    auto fields = tuple->getElements();

    SmallVector<RValue, 4> elements;
    std::move(src).extractElements(elements);
    for (unsigned i = 0, n = fields.size(); i != n; ++i) {
      emitMemberInit(SGF, selfDecl, fields[i].getPattern(),
                     std::move(elements[i]));
    }
    break;
  }

  case PatternKind::Named: {
    auto named = cast<NamedPattern>(pattern);
    // Form the lvalue referencing this member.
    WritebackScope scope(SGF);
    SILLocation loc = pattern;
    ManagedValue self;
    CanType selfFormalType = selfDecl->getType()
        ->getInOutObjectType()->getCanonicalType();
    if (selfFormalType->hasReferenceSemantics())
      self = SGF.emitRValueForDecl(loc, selfDecl, selfDecl->getType(),
                                   AccessSemantics::DirectToStorage,
                                   SGFContext::AllowImmediatePlusZero);
    else
      self = SGF.emitLValueForDecl(loc, selfDecl, src.getType(),
                                   AccessKind::Write,
                                   AccessSemantics::DirectToStorage);

    LValue memberRef =
      SGF.emitPropertyLValue(loc, self, selfFormalType, named->getDecl(),
                             AccessKind::Write,
                             AccessSemantics::DirectToStorage);

    // Assign to it.
    SGF.emitAssignToLValue(loc, std::move(src), std::move(memberRef));
    return;
  }

  case PatternKind::Any:
    return;

  case PatternKind::Typed:
    return emitMemberInit(SGF, selfDecl,
                          cast<TypedPattern>(pattern)->getSubPattern(),
                          std::move(src));

  case PatternKind::Var:
    return emitMemberInit(SGF, selfDecl,
                          cast<VarPattern>(pattern)->getSubPattern(),
                          std::move(src));

#define PATTERN(Name, Parent)
#define REFUTABLE_PATTERN(Name, Parent) case PatternKind::Name:
#include "swift/AST/PatternNodes.def"
    llvm_unreachable("Refutable pattern in pattern binding");
  }
}

void SILGenFunction::emitMemberInitializers(VarDecl *selfDecl,
                                            NominalTypeDecl *nominal) {
  for (auto member : nominal->getMembers()) {
    // Find pattern binding declarations that have initializers.
    auto pbd = dyn_cast<PatternBindingDecl>(member);
    if (!pbd || pbd->isStatic()) continue;

    for (auto entry : pbd->getPatternList()) {
      auto init = entry.getInit();
      if (!init) continue;

      // Cleanup after this initialization.
      FullExpr scope(Cleanups, entry.getPattern());
      emitMemberInit(*this, selfDecl, entry.getPattern(), emitRValue(init));
    }
  }
}

void SILGenFunction::emitIVarInitializer(SILDeclRef ivarInitializer) {
  auto cd = cast<ClassDecl>(ivarInitializer.getDecl());
  RegularLocation loc(cd);
  loc.markAutoGenerated();

  // Emit 'self', then mark it uninitialized.
  auto selfDecl = cd->getDestructor()->getImplicitSelfDecl();
  SILType selfTy = getLoweredLoadableType(selfDecl->getType());
  SILValue selfArg = new (SGM.M) SILArgument(F.begin(), selfTy, selfDecl);
  SILLocation PrologueLoc(selfDecl);
  PrologueLoc.markAsPrologue();
  B.createDebugValue(PrologueLoc, selfArg);
  selfArg = B.createMarkUninitialized(selfDecl, selfArg,
                                      MarkUninitializedInst::RootSelf);
  assert(selfTy.hasReferenceSemantics() && "can't emit a value type ctor here");
  VarLocs[selfDecl] = VarLoc::get(selfArg);

  auto cleanupLoc = CleanupLocation::get(loc);
  prepareEpilog(TupleType::getEmpty(getASTContext()), false, cleanupLoc);

  // Emit the initializers.
  emitMemberInitializers(cd->getDestructor()->getImplicitSelfDecl(), cd);

  // Return 'self'.
  B.createReturn(loc, selfArg);

  emitEpilog(loc);
}
