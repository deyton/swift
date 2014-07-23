// RUN: %swift %s -o /dev/null -emit-silgen -verify


struct MutatorStruct {
  mutating func f(inout x : MutatorStruct) {}
}

var global_property : MutatorStruct { get {} set {} }

var global_int_property : Int {
get { return 42 }
set {}
}


struct StructWithProperty {
  var computed_int : Int {
  get { return 42 }
  set {}
  }
  
  var stored_int = 0
  
  var computed_struct : MutatorStruct { get {} set {} }
}

var global_struct_property : StructWithProperty
var c_global_struct_property : StructWithProperty { get {} set {} }

func testWriteback() {
  var a = StructWithProperty()
  a.computed_struct .        // expected-note {{concurrent writeback occurred here}}
     f(&a.computed_struct)  // expected-error {{inout writeback to computed property 'computed_struct' occurs in multiple arguments to call, introducing invalid aliasing}}
  
  swap(&global_struct_property.stored_int,
       &global_struct_property.stored_int) // ok
  swap(&global_struct_property.computed_int,  // expected-note {{concurrent writeback occurred here}}
       &global_struct_property.computed_int)  // expected-error {{inout writeback to computed property 'computed_int' occurs in multiple arguments to call, introducing invalid aliasing}}
  
  
  
  swap(&a.computed_int,   // expected-note {{concurrent writeback occurred here}}
       &a.computed_int)   // expected-error {{inout writeback to computed property 'computed_int' occurs in multiple arguments to call, introducing invalid aliasing}}
  
  
  var stored_local = 42
  swap(&stored_local, &stored_local)  // ok, physical lvalue.
  
  global_property.f(&global_property) // expected-error {{inout writeback to computed property 'global_property' occurs in multiple arguments to call, introducing invalid aliasing}} expected-note {{concurrent writeback occurred here}}
  
  a.computed_struct.f(&a.computed_struct)  // expected-error {{inout writeback to computed property 'computed_struct' occurs in multiple arguments to call, introducing invalid aliasing}} expected-note {{concurrent writeback occurred here}}
}

func testComputedStructWithProperty() {
  swap(&c_global_struct_property.stored_int, &c_global_struct_property.stored_int)   // expected-error {{inout writeback to computed property 'c_global_struct_property' occurs in multiple arguments to call, introducing invalid aliasing}} expected-note {{concurrent writeback occurred here}}
  
  var c_local_struct_property : StructWithProperty { get {} set {} }
  swap(&c_local_struct_property.stored_int, &c_local_struct_property.stored_int)    // expected-error {{inout writeback to computed property 'c_local_struct_property' occurs in multiple arguments to call, introducing invalid aliasing}} expected-note {{concurrent writeback occurred here}}
  swap(&c_local_struct_property.stored_int, &c_global_struct_property.stored_int) // ok
}


var global_array : [[Int]]

func testMultiArray(i : Int, j : Int, var array : [[Int]]) {
  swap(&array[i][j],  // expected-note  {{concurrent writeback occurred here}}
       &array[i][i])  // expected-error {{inout writeback through subscript occurs in multiple arguments to call, introducing invalid aliasing}}
  // FIXME: Handle subscripts.
  swap(&array[0][j], &array[0][i])
  swap(&global_array[0][j], &global_array[0][i])
  swap(&global_array[0][j], &array[j][i])  // ok
}

