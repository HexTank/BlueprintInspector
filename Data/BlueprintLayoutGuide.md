# Blueprint layout spec

How to lay the opcode fixtures out in real Blueprint graphs so the compiler produces the bytecode
under test. This document stops at the bytecode. Decoding is out of scope here.

The CSV in §3 defines the fixtures. §2 lists the assets and members every row
depends on. `Content/Tests/OpcodeShowcase/BP_OpcodeDemo_All` contains 82 of
the 98 rows. The graph generator archives every non-empty function graph under
`Content/Tests/OpcodeShowcase/Graphs` and rebuilds the graphs from that archive. Run it with
`-BlueprintFixtureTools`. The automation tests in
`Private/Tests/BlueprintInspectorAutomationTests.cpp` walk the compiled functions and compare their
captured opcode sequences with this CSV.

## 1. Layout rules

- **R1. One fixture = one function graph, named exactly `fixture_id`.** The function name is the join
  key between the CSV and the compiled `UFunction`.
- **R2. Fixture functions are split by what a transient compiler can author.** `BP_OpcodeDemo_All`
  carries every archive row except `const_struct`, `structconst_containers` and `let_struct` (which
  live in `BP_OpcodeDemo_Structs`, because a valid struct default re-instances the user structs).
  Variables, dispatchers, structs and the enum are declared once.
- **R3. Lay each graph left to right in exec order, one linear chain, inside a comment box titled with
  `fixture_id`.** The comment box makes the graph-to-CSV mapping clear.
- **R4. Every non-exec output pin must be consumed.** A pure node with an unwired output is dropped
  by the compiler and emits no bytecode.
- **R5. The literal lives on a pin default, never in a temporary.** `EmitTermExpr` chooses the token
  from the pin's type.
- **R6. The variable kind picks the token.** A member variable gives `EX_InstanceVariable`. A function
  parameter gives `EX_LocalVariable`. The return property gives `EX_LocalOutVariable`. A
  class-default context gives `EX_DefaultVariable`.
- **R7. Events go in the Event Graph, functions in function graphs.** Only an Event entry produces
  the generated `ExecuteUbergraph_*` stub and `EX_ComputedJump`. Only an event with data output pins
  produces `EX_LetValueOnPersistentFrame`.
- **R8. Container shapes differ by wiring, not by node.** A container wired into a variable is the
  statement form (`EX_SetArray … EX_EndArray`). The same literal as a disconnected parameter default
  is the term form (`EX_ArrayConst … EX_EndArrayConst`).
- **R9. Impure and pure casts cover different paths.** An impure cast yields `EX_Let` +
  `EX_DynamicCast` + `EX_Let` + `EX_Cast` + `EX_JumpIfNot` + `EX_Jump`. A pure cast yields the first
  two only.
- **R10. Call Parent Function lives in a child Blueprint.** The fixture asset itself derives from
  `BP_ProbeParent` and overrides `F_ParentTarget`.
- **R11. Some tokens have no node of their own.** `EX_Context*` comes from any member call,
  `EX_ComputedJump` from any event, `EX_Jump` from a Sequence or Branch, `EX_ArrayGetByRef` from GET
  off an array pin. Assert on `Receiver`, `InputNodes` and `WiresToProcess`, not on text.
- **R12. Keep fixture functions small (about 5 to 15 nodes).** The node pool is 1024 and unchecked.
  `Make Map` with many pairs burns nodes fast.
- **R13. One fixture per function.** Never share a node or intermediate value between fixtures.
- **R14. Statement position is not layable for casts, `EX_SwitchValue` or `EX_ArrayGetByRef`.**
  Those shapes belong to synthesized bytecode.
- **R15. Debug/instrumentation tokens are a build setting, not a layout.** Observe them
  interactively. Do not assert on them from the automation suite.
- **R16. The tools compact the layout.** `CompactLayout` pulls each graph to the origin, caps column
  gaps at 260 px and row gaps at 180 px, and places the Function Entry and Result at the two ends, so
  a generated fixture stays inside a window.

## 2. Asset and prerequisite spec

`id` values are what the CSV's `asset` column resolves against. The fixture host keeps the CSV key
`NewBlueprint`, but the saved asset is `BP_OpcodeDemo_All`.

```json
{
  "spec_version": 1,
  "fixture_root": "/BlueprintInspector/Tests/OpcodeShowcase",
  "assets": [
    {
      "id": "NewBlueprint",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/BP_OpcodeDemo_All",
      "type": "Blueprint (Actor)",
      "parent_class": "BP_ProbeParent",
      "role": "fixture host - every F_* fixture function graph lives here",
      "notes": "The fixture asset is also the child class for Call Parent Function."
    },
    {
      "id": "BP_ProbeParent",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/BP_ProbeParent",
      "type": "Blueprint (Actor)",
      "parent_class": "AActor",
      "role": "parent for EX_FinalFunction via Call Parent Function",
      "functions": [
        { "name": "F_ParentTarget", "params": [{ "name": "Value", "type": "int32" }], "returns": "void" }
      ]
    },
    {
      "id": "BP_ProbeCastTarget",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/BP_ProbeCastTarget",
      "type": "Blueprint (Actor)",
      "parent_class": "AActor",
      "role": "empty class used as the target of Cast To <Class> nodes",
      "members": [{ "name": "ProbeInt", "type": "int32" }]
    },
    {
      "id": "BFL_ProbeLibrary",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/BFL_ProbeLibrary",
      "type": "Blueprint Function Library",
      "role": "supplies the container-typed parameter defaults that produce the EX_*Const term forms (R8)",
      "functions": [
        { "name": "DefaultArray", "params": [{ "name": "Ints", "type": "TArray<int32>", "default": [1, 2, 3] }], "returns": "void" },
        { "name": "DefaultSet", "params": [{ "name": "Names", "type": "TSet<FName>", "default": ["Alpha", "Beta"] }], "returns": "void" },
        { "name": "DefaultMap", "params": [{ "name": "Lookup", "type": "TMap<int32,FName>", "default": { "1": "Alpha", "2": "Beta" } }], "returns": "void" },
        { "name": "DefaultStruct", "params": [{ "name": "Value", "type": "S_ProbeContainers", "default": "container members populated" }], "returns": "void" },
        { "name": "SoftRef", "params": [{ "name": "Asset", "type": "TSoftObjectPtr<UObject>", "default": "/BlueprintInspector/Tests/OpcodeShowcase/M_Probe" }], "returns": "void" },
        { "name": "SingleDelegate", "params": [{ "name": "Del", "type": "D_ProbeSingle", "default": "(unset)" }], "returns": "void" }
      ]
    },
    {
      "id": "BPI_ProbeA",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/BPI_ProbeA",
      "type": "Blueprint Interface",
      "role": "interface-typed pin and interface message call",
      "functions": [{ "name": "IProbeFunc", "params": [{ "name": "Input", "type": "int32" }], "returns": "void" }]
    },
    {
      "id": "BPI_ProbeB",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/BPI_ProbeB",
      "type": "Blueprint Interface",
      "role": "destination of the interface-to-interface cast",
      "functions": [{ "name": "IProbeOther", "params": [{ "name": "Input", "type": "int32" }], "returns": "int32" }]
    },
    {
      "id": "E_ProbeEnum",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/E_OpcodeDemoEnum",
      "type": "Enumeration",
      "values": ["One", "Two", "Three"],
      "role": "enum literal pin - asserts EX_ByteConst rather than EX_IntConst"
    },
    {
      "id": "S_ProbeNested",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/S_ProbeNested",
      "type": "Structure",
      "members": [{ "name": "Flag", "type": "bool" }]
    },
    {
      "id": "S_ProbeStruct",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/S_ProbeStruct",
      "type": "Structure",
      "members": [
        { "name": "Int", "type": "int32" },
        { "name": "Real", "type": "real" },
        { "name": "Str", "type": "FString" },
        { "name": "Obj", "type": "UObject*" },
        { "name": "Nested", "type": "S_ProbeNested" }
      ],
      "role": "EX_StructConst / EX_EndStructConst, and a member of a struct-literal pin"
    },
    {
      "id": "S_ProbeContainers",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/S_ProbeContainers",
      "type": "Structure",
      "members": [
        { "name": "Ints", "type": "TArray<int32>" },
        { "name": "Names", "type": "TSet<FName>" },
        { "name": "Lookup", "type": "TMap<int32,FName>" }
      ],
      "role": "container members inside a struct literal - the EX_*Const values nested in EX_StructConst"
    },
    {
      "id": "M_Probe",
      "path": "/BlueprintInspector/Tests/OpcodeShowcase/M_Probe",
      "type": "Material",
      "role": "target of EX_ObjectConst and EX_SoftObjectConst literal pins"
    }
  ],

  "delegate_signatures": [
    { "id": "D_ProbeSingle", "type": "single-cast", "params": [{ "name": "Value", "type": "int32" }] },
    { "id": "D_ProbeMulticast", "type": "multicast", "params": [{ "name": "Value", "type": "int32" }] }
  ],

  "variables_on_NewBlueprint": [
    { "name": "IntVar", "type": "int32" },
    { "name": "Int64Var", "type": "int64" },
    { "name": "ByteVar", "type": "byte" },
    { "name": "EnumVar", "type": "E_ProbeEnum" },
    { "name": "BoolVar", "type": "bool" },
    { "name": "RealVar", "type": "real" },
    { "name": "StringVar", "type": "FString" },
    { "name": "NameVar", "type": "FName" },
    { "name": "TextVar", "type": "FText" },
    { "name": "ObjectVar", "type": "UObject*" },
    { "name": "ActorVar", "type": "AActor*" },
    { "name": "ClassVar", "type": "TSubclassOf<AActor>" },
    { "name": "SoftObjVar", "type": "TSoftObjectPtr<UObject>" },
    { "name": "WeakObjVar", "type": "TWeakObjectPtr<AActor>" },
    { "name": "InterfaceVar", "type": "BPI_ProbeA (interface reference)" },
    { "name": "StructVar", "type": "S_ProbeStruct" },
    { "name": "ContainerStructVar", "type": "S_ProbeContainers" },
    { "name": "ArrayVar", "type": "TArray<int32>" },
    { "name": "SetVar", "type": "TSet<FName>" },
    { "name": "MapVar", "type": "TMap<int32,FName>" },
    { "name": "VectorVar", "type": "FVector" },
    { "name": "RotatorVar", "type": "FRotator" },
    { "name": "TransformVar", "type": "FTransform" },
    { "name": "SingleDelegateVar", "type": "FProbeSingleDelegateSignature (native on UOpcodeProbeHost)" },
    { "name": "MulticastVar", "type": "D_ProbeMulticast" }
  ],

  "dispatchers_on_NewBlueprint": [
    { "name": "OnProbeMulticast", "type": "D_ProbeMulticast", "role": "Bind Event / Unbind Event / Unbind all Events / Call Dispatcher" }
  ],

  "functions_on_NewBlueprint": [
    { "name": "F_ProbeLocalFunc", "params": [], "returns": "void", "role": "callee for EX_LocalFinalFunction - keep it trivial" },
    { "name": "F_ProbeOverridable", "params": [], "returns": "void", "role": "EX_LocalVirtualFunction attempt" },
    { "name": "F_ProbeDelegateHandler", "params": [{ "name": "Value", "type": "int32" }], "returns": "void", "role": "target selected by Create Event for EX_BindDelegate" }
  ],

  "events_on_NewBlueprint": [
    { "name": "E_WithDataOut", "params": [], "outputs": [{ "name": "Out", "type": "int32" }], "role": "event with data output pins - the only route to EX_LetValueOnPersistentFrame" },
    { "name": "E_Any", "params": [], "outputs": [], "role": "any Event - the generated ExecuteUbergraph_* stub is the only route to EX_ComputedJump" }
  ],

  "native_surface": {
    "required": false,
    "note": "Only the native_only rows need this. Adding it means touching C++ in the game module or the plugin.",
    "class": {
      "name": "UOpcodeProbeHost",
      "properties": [
        { "name": "FloatVar", "type": "float", "role": "EX_FloatConst - Blueprint real pins are double" },
        { "name": "UInt64Var", "type": "uint64", "role": "EX_UInt64Const - uint64 is not a Blueprint pin type" },
        { "name": "Vector3fVar", "type": "FVector3f", "role": "EX_Vector3fConst - FVector3f is not a Blueprint pin type" },
        { "name": "FieldPathVar", "type": "FFieldPath", "role": "EX_FieldPathConst - no stock palette node exposes an editable FFieldPath literal" }
      ],
      "functions": [
        { "name": "GetSinglePrecision", "returns": "float", "role": "CST_FloatToDouble when its output feeds a real pin; the reverse gives CST_DoubleToFloat" }
      ],
      "sparse_class_data": "a UCLASS using sparse class data, for EX_ClassSparseDataVariable"
    }
  }
}
```

## 3. Fixture spec (CSV)

Conventions:

- `fixture_id` is the function graph name (R1).
- Fields are comma-separated. A semicolon separates list items inside a field.
- `asset` and `graph` resolve against §2.
- `opcodes` lists the tokens the layout is responsible for, in emission order. Every function also
  ends with `EX_Return;EX_Nothing;EX_EndOfScript` (void) or
  `EX_Return;EX_LocalOutVariable;<return-property>;EX_EndOfScript` (valued).
- `render` vocabulary: `ok`, `quoted-name` (bare `FName`), `expr-default` (`Name(inputs)`),
  `stmt-default` (`Name(inputs);`), `nothing`, `n/a`.
- `gap` says what the fixture should expose. `none` means a regression guard.

```csv
fixture_id,family,shape,asset,graph,node,node_class,pin_types,literals,wiring,opcodes,decoder,render,gap
let_int,4.1 Assignment,function,NewBlueprint,F_let_int,Set IntVar,UK2Node_VariableSet,int,7,exec entry -> Set; lit(Set.value)=7,EX_Let;EX_InstanceVariable;EX_IntConst,§4.1,ok,none
let_real,4.1 Assignment,function,NewBlueprint,F_let_real,Set RealVar,UK2Node_VariableSet,real,1.5,exec entry -> Set; lit(Set.value)=1.5,EX_Let;EX_InstanceVariable;EX_DoubleConst,§4.1,ok,none
let_string_ascii,4.1 Assignment,function,NewBlueprint,F_let_string_ascii,Set StringVar,UK2Node_VariableSet,string,probe,exec entry -> Set; lit(Set.value)=probe,EX_Let;EX_InstanceVariable;EX_StringConst,§4.1,ok,none
let_string_unicode,4.1 Assignment,function,NewBlueprint,F_let_string_unicode,Set StringVar,UK2Node_VariableSet,string,héllo (contains a non-ASCII char),exec entry -> Set; lit(Set.value)=héllo,EX_Let;EX_InstanceVariable;EX_UnicodeStringConst,§4.1,ok,One non-ASCII character switches the token from EX_StringConst to EX_UnicodeStringConst
let_text,4.1 Assignment,function,NewBlueprint,F_let_text,Set TextVar,UK2Node_VariableSet,text,Invariant text literal,exec entry -> Set; lit(Set.value)=text,EX_Let;EX_InstanceVariable;EX_TextConst,§4.1,ok,Invariant and LiteralString push the string; LocalizedText pushes its source; StringTableEntry pushes its key; Empty pushes an empty name
let_name,4.1 Assignment,function,NewBlueprint,F_let_name,Set NameVar,UK2Node_VariableSet,name,ProbeName,exec entry -> Set; lit(Set.value)=ProbeName,EX_Let;EX_InstanceVariable;EX_NameConst,§4.1,ok,none
let_enum,4.1 Assignment,function,NewBlueprint,F_let_enum,Set EnumVar,UK2Node_VariableSet,byte (E_ProbeEnum),E_ProbeEnum.Two,exec entry -> Set; lit(Set.value)=Two,EX_Let;EX_InstanceVariable;EX_ByteConst,§4.1,ok,Asserts EX_ByteConst and not EX_IntConst for an enum literal
let_bool,4.1 Assignment,function,NewBlueprint,F_let_bool,Set BoolVar,UK2Node_VariableSet,bool,true,exec entry -> Set; lit(Set.value)=true,EX_LetBool;EX_InstanceVariable;EX_True,§4.1,ok,none
let_obj,4.1 Assignment,function,NewBlueprint,F_let_obj,Set ObjectVar,UK2Node_VariableSet,object ref,M_Probe,exec entry -> Set; lit(Set.value)=M_Probe,EX_LetObj;EX_InstanceVariable;EX_ObjectConst,§4.1,ok,The object operand renders its name; a null object renders as an empty string
let_class,4.1 Assignment,function,NewBlueprint,F_let_class,Set ClassVar,UK2Node_VariableSet,class ref,BP_ProbeCastTarget,exec entry -> Set; lit(Set.value)=BP_ProbeCastTarget,EX_LetObj;EX_InstanceVariable;EX_ObjectConst,§4.1,ok,none
let_struct,4.1 Assignment,function,NewBlueprint,F_let_struct,Set StructVar,UK2Node_VariableSet,struct (S_ProbeStruct),member literals,exec entry -> Set; lit each member pin,EX_Let;EX_InstanceVariable;EX_StructConst;EX_EndStructConst,§4.1,ok,EX_StructConst is a term - lifted in Input and rendered as Name(...)
let_softobj,4.1 Assignment,function,NewBlueprint,F_let_softobj,Set SoftObjVar,UK2Node_VariableSet,soft object ref,M_Probe,exec entry -> Set; lit(Set.value)=M_Probe,EX_Let;EX_InstanceVariable;EX_SoftObjectConst,§4.1,ok,Nested string payload is safe; the payload renders as a bare quoted FName
let_array,4.1 Assignment,function,NewBlueprint,F_let_array,Set ArrayVar,UK2Node_VariableSet,array<int32>,"1, 2",exec entry -> Set; Make Array feeding Set.value,EX_SetArray;EX_LocalVariable;EX_IntConst;EX_IntConst;EX_EndArray;EX_Let;EX_InstanceVariable;EX_LocalVariable,§4.1,ok,The container block leads and the destination follows the Let - the compiler builds the array into a local temp first
let_vector,4.1 Assignment,function,NewBlueprint,F_let_vector,Set VectorVar,UK2Node_VariableSet,vector,X=1; Y=2; Z=3,exec entry -> Set; lit(Set.value) X/Y/Z,EX_Let;EX_InstanceVariable;EX_VectorConst,§4.1,ok,Renders through the FVector* operand overload
let_rotator,4.1 Assignment,function,NewBlueprint,F_let_rotator,Set RotatorVar,UK2Node_VariableSet,rotator,Pitch=0; Yaw=90; Roll=0,exec entry -> Set; lit(Set.value) P/Y/R,EX_Let;EX_InstanceVariable;EX_RotationConst,§4.1,ok,"Pin default must use the (X=..,Y=..,Z=..) form; renders through the FRotator* overload"
let_transform,4.1 Assignment,function,NewBlueprint,F_let_transform,Set TransformVar,UK2Node_VariableSet,transform,identity,exec entry -> Set; lit(Set.value)=identity,EX_Let;EX_InstanceVariable;EX_TransformConst,§4.1,ok,Renders via the FTransform* operand overload
let_weakobj,4.1 Assignment,function,NewBlueprint,F_let_weakobj,Set WeakObjVar,UK2Node_VariableSet,weak object ref,None,exec entry -> Set; data ActorVar -> Set.value,EX_LetWeakObjPtr;EX_InstanceVariable;EX_InstanceVariable,§4.1,ok,"WeakObjVar is a real weak object property. AddMemberVariable clears bIsWeakPointer for user-created variables, so the fixture tool restores the flag on the variable and on the destination pin; the compiler then emits EX_LetWeakObjPtr"
let_delegate,4.1 Assignment,function,NewBlueprint,F_let_delegate,Assign SingleDelegateVar,UK2Node_VariableSet,single-cast delegate,Create Event -> F_ProbeDelegateHandler,exec entry -> Assign; data Create Event -> Assign.value,EX_BindDelegate;EX_LetDelegate;EX_InstanceVariable,§4.1,ok,A delegate-typed VariableSet: UK2Node_DelegateSet is an event entry point and cannot live in a function body; the compiler binds into a temp before the assignment
let_multicast,4.1 Assignment,function,NewBlueprint,F_let_multicast,Assign MulticastVar,UK2Node_DelegateSet,multicast delegate,Create Event -> F_ProbeDelegateHandler,exec entry -> Assign; data Create Event -> Assign.value,EX_LetMulticastDelegate;EX_InstanceVariable,§4.1,ok,Which Let token appears follows the destination pin type only
let_persistent_frame,4.1 Assignment,event_ubergraph,NewBlueprint,EventGraph,Custom event E_WithDataOut with an int output pin,UK2Node_Event,int output pin,drag off the output pin,exec entry -> node consuming E_WithDataOut.Out,EX_LetValueOnPersistentFrame,§4.1,ok,"Captured from the E_WithDataOut event stub; the stub stores the output in the persistent ubergraph frame before it calls ExecuteUbergraph"
callmath_add,4.2 Calls,function,NewBlueprint,F_callmath_add,Add (float variant),UK2Node_CallFunction,float,"1.0, 2.0",exec entry -> Add; lit(Add.A)=1.0; lit(Add.B)=2.0,EX_CallMath;EX_EndFunctionParms,§4.2,ok,Pick the float variant; a wildcard resolving to double gives EX_DoubleConst operands
callmath_abs_int,4.2 Calls,function,NewBlueprint,F_callmath_abs_int,Abs (int variant),UK2Node_CallFunction,int,-5,exec entry -> Abs; lit(Abs.A)=-5,EX_CallMath;EX_IntConst;EX_EndFunctionParms,§4.2,ok,none
finalcall_parent,4.2 Calls,child_function,BP_OpcodeDemo_All,F_finalcall_parent,Call Parent Function (F_ParentTarget),UK2Node_CallParentFunction,int,7,exec entry -> CallParent; lit(Value)=7,EX_LocalFinalFunction;EX_IntConst;EX_EndFunctionParms,§4.2,ok,The parent call is final and a local script function so it emits EX_LocalFinalFunction. EX_FinalFunction needs a final callee that is not a local script function. The fixture asset itself derives from BP_ProbeParent
localfinal_bpfunc,4.2 Calls,function,NewBlueprint,F_localfinal_bpfunc,call F_ProbeLocalFunc,UK2Node_CallFunction,none,none,exec entry -> Call F_ProbeLocalFunc,EX_LocalVirtualFunction;EX_EndFunctionParms;EX_Return;EX_Nothing,§4.2,ok,"F_ProbeLocalFunc is a non-native Blueprint function and is not final so the call emits EX_LocalVirtualFunction. No Blueprint-authored function is final. The caller's bytecode does not describe the callee's body, so the walk stops at the call"
localvirtual_bpfunc,4.2 Calls,function,NewBlueprint,F_localvirtual_bpfunc,call F_ProbeOverridable,UK2Node_CallFunction,none,none,exec entry -> call; also wire the same call from a child of BP_ProbeParent,EX_LocalVirtualFunction;EX_EndFunctionParms,§4.2,ok,Which local-call token appears is the compiler's bLocalScriptFunction decision; assert what the script records
virtual_function,4.2 Calls,function,NewBlueprint,F_virtual_function,Get Velocity,UK2Node_CallFunction,object ref (self),Target = self,exec entry -> Get Velocity; data self -> Target,EX_VirtualFunction;EX_EndFunctionParms,§4.2,ok,Needs a real owner class - the fixture Blueprint supplies it
context_getactorlocation,4.2 Calls,function,NewBlueprint,F_context_getactorlocation,Get Actor Location,UK2Node_CallFunction,actor ref,Target = ActorVar,exec entry -> Get Actor Location; data ActorVar -> Target,EX_Context;EX_InstanceVariable;EX_FinalFunction;EX_EndFunctionParms,§4.2,ok,Get Actor Location is a native non-virtual function so the call is EX_FinalFunction. The context builds a Context node. Assert on node.Receiver and node.InputNodes
context_failsilent,4.2 Calls,function,NewBlueprint,F_context_failsilent,Set Actor Scale 3D,UK2Node_CallFunction,actor ref,Target = ActorVar,exec entry -> Set Actor Scale 3D; data ActorVar -> Target,EX_Context_FailSilent;EX_InstanceVariable;EX_FinalFunction;EX_VectorConst;EX_EndFunctionParms,§4.2,ok,"The test run passes -ini:Engine:[Kismet]:bCanSuppressAccessViolation=true; the compiler caches the setting at the first context emission, so it has to be on the command line. Set Actor Scale 3D is native, non-virtual and returns void, so bUnsafeToSkip stays false and the context emits EX_Context_FailSilent; a call with a return value or a non-const out parameter still emits EX_Context"
call_selfarg,4.2 Calls,function,NewBlueprint,F_call_selfarg,A call whose first argument is the caller,UK2Node_CallFunction,object,self -> Caller; lit(Value)=7,data self -> Caller,EX_CallMath;EX_Self,§4.2,ok,The callee declares no WorldContext meta so the caller stays its first argument
context_output_lvalue,4.2 Calls,function,NewBlueprint,F_context_output_lvalue,an l-value member assignment of the shape Object.Member = value,UK2Node_VariableSet,struct member,struct literal,exec entry -> Set; dest is a context-shaped property path,EX_Let;EX_Context;EX_InstanceVariable;EX_InstanceVariable;EX_IntConst,§4.2,ok,"An assignment emits no call context so there is no FailSilent. The Let destination is a Context over CastTargetVar with ProbeInt under it. The unwired value pin pushes 0. EX_StructMemberContext sets ReturnNode and EX_Context adds the receiver prefix so the target renders named"
classcontext_getclassdefaults,4.2 Calls,function,NewBlueprint,F_classcontext_getclassdefaults,Get Class Defaults,UK2Node_GetClassDefaults,class ref,BP_ProbeCastTarget,exec entry -> Get Class Defaults; data BP_ProbeCastTarget -> Class,EX_ClassContext;EX_DefaultVariable,§4.2,ok,The only caller of SetContextTypeClass() - no node is created
interfacecontext_messagecall,4.2 Calls,function,NewBlueprint,F_interfacecontext_messagecall,interface message call,UK2Node_Message,int (param),InterfaceVar holds the interface ref; Input=7,exec entry -> IProbeFunc; data InterfaceVar -> Target; lit(Input)=7,EX_Context_FailSilent;EX_InterfaceContext;EX_LocalVariable;EX_LocalVirtualFunction;EX_IntConst;EX_EndFunctionParms,§4.2,ok,"The interface function is non-native so bLocalScriptFunction is set and the call is EX_LocalVirtualFunction. EX_InterfaceContext adds no node and decoding continues with the next token. The return value is unused and the test run passes -ini:Engine:[Kismet]:bCanSuppressAccessViolation=true, so the context emits EX_Context_FailSilent. Interface receivers resolve through the interface-class fallback and the call renders as Receiver->IProbeFunc(7)"
var_instance,4.3 Variables,function,NewBlueprint,F_var_instance,Get IntVar,UK2Node_VariableGet,int,None,Get IntVar -> data into Set IntVar,EX_InstanceVariable,§4.3,ok,Member variable on the fixture class - the R6 distinction
var_local,4.3 Variables,function,NewBlueprint,F_var_local,read the fixture function's own int input parameter,UK2Node_FunctionEntry,int parameter,Input=5 (the function's parameter),parameter read -> data into Set IntVar,EX_LocalVariable,§4.3,ok,Must be a parameter of the fixture function itself; a member variable gives EX_InstanceVariable
var_localout,4.3 Variables,function,NewBlueprint,F_var_localout,a fixture function that returns a value,UK2Node_FunctionResult,none,none,Return Node of F_var_localout,EX_Return;EX_LocalOutVariable,§4.3,ok,The return property is the only node-free producer of this token - covered by flow_return_value too
none_object,4.3 Variables,function,NewBlueprint,F_none_object,any object input pin left at None,UK2Node_CallFunction,object ref,None,data ObjectVar=None -> Set ObjectVar,EX_NoObject,§4.3,ok,none
none_interface,4.3 Variables,function,NewBlueprint,F_none_interface,any interface input pin left at None,UK2Node_CallFunction,interface ref,None,data InterfaceVar=None -> Set InterfaceVar,EX_NoInterface,§4.3,ok,An operand token with no case - renders gracefully as EExprToken(0x2D)
self_node,4.3 Variables,function,NewBlueprint,F_self_node,Self,UK2Node_Self,object ref,None,Self -> data into Set ObjectVar,EX_Self,§4.3,ok,none
var_sparsedata,4.3 Variables,native_only,NewBlueprint,,sparse class data member read,,sparse class data member,None,read the member,EX_ClassSparseDataVariable,§4.3,ok,Requires a C++ UCLASS using sparse class data; version-guarded in the decoder
const_int,4.4 Constants,function,NewBlueprint,F_const_int,literal pin on any int input,UK2Node_CallFunction,int,-7,lit(target.Input)=-7,EX_IntConst,§4.4,ok,none
const_int64,4.4 Constants,function,NewBlueprint,F_const_int64,literal pin on any int64 input,UK2Node_CallFunction,int64,9007199254740993,lit(target.Input)=9007199254740993,EX_Int64Const,§4.4,ok,none
const_uint64,4.4 Constants,native_only,NewBlueprint,,literal pin on a uint64 input,,uint64,1,lit(target.Input)=1,EX_UInt64Const,§4.4,ok,uint64 is not a Blueprint pin type - native API only
const_byte,4.4 Constants,function,NewBlueprint,F_const_byte,literal pin on any byte input,UK2Node_CallFunction,byte,200,lit(target.Input)=200,EX_ByteConst,§4.4,ok,none
const_float,4.4 Constants,native_only,NewBlueprint,,literal pin on a C++ float input,,float,0.5,lit(target.Input)=0.5,EX_FloatConst,§4.4,ok,EX_FloatConst needs a C++ float - Blueprint real pins are double
const_double,4.4 Constants,function,NewBlueprint,F_const_double,literal pin on a real input,UK2Node_CallFunction,real,0.5,lit(target.Input)=0.5,EX_DoubleConst,§4.4,ok,The UE5 default for a Blueprint numeric literal
const_string_ascii,4.4 Constants,function,NewBlueprint,F_const_string_ascii,literal pin on a string input,UK2Node_CallFunction,string,probe,lit(target.Input)=probe,EX_StringConst,§4.4,ok,Pin must be String and not Text
const_string_unicode,4.4 Constants,function,NewBlueprint,F_const_string_unicode,literal pin on a string input,UK2Node_CallFunction,string,héllo,lit(target.Input)=héllo,EX_UnicodeStringConst,§4.4,ok,One non-ASCII character switches the token
const_text_localized,4.4 Constants,function,NewBlueprint,F_const_text_localized,literal pin on a text input,UK2Node_CallFunction,text,Localized text literal,lit(target.Input)=localized text,EX_TextConst,§4.4,ok,LocalizedText pushes its source string; StringTableEntry pushes its key
const_name,4.4 Constants,function,NewBlueprint,F_const_name,literal pin on a name input,UK2Node_CallFunction,name,ProbeName,lit(target.Input)=ProbeName,EX_NameConst,§4.4,ok,none
const_object,4.4 Constants,function,NewBlueprint,F_const_object,object literal pin,UK2Node_Literal,object ref,M_Probe,lit(target.Input)=M_Probe,EX_LetObj;EX_InstanceVariable;EX_InstanceVariable,§4.4,ok,The built asset emits no EX_ObjectConst. The literal node's object merges into a member reference so both sides read as EX_InstanceVariable. let_obj covers EX_ObjectConst
const_softobject,4.4 Constants,function,NewBlueprint,F_const_softobject,soft object literal pin,UK2Node_CallFunction,soft object ref,M_Probe,lit(target.Input)=M_Probe,EX_SoftObjectConst,§4.4,quoted-name,The payload stays a bare FName; the nested string path is safe (C9)
const_fieldpath,4.4 Constants,native_only,NewBlueprint,,literal pin on an FFieldPath input,,FFieldPath,path,lit(target.Input)=path,EX_FieldPathConst,§4.4,quoted-name,No stock palette node exposes an editable FFieldPath literal - needs a purpose-built UFUNCTION
const_vector,4.4 Constants,function,NewBlueprint,F_const_vector,vector literal pin,UK2Node_CallFunction,vector,X=1; Y=2; Z=3,lit(target.Input) X/Y/Z,EX_VectorConst,§4.4,ok,LWC - element size is double on 5.5.4
const_vector3f,4.4 Constants,native_only,NewBlueprint,,literal pin on an FVector3f input,,FVector3f,X=1; Y=2; Z=3,lit(target.Input) X/Y/Z,EX_Vector3fConst,§4.4,ok,FVector3f is not a Blueprint pin type - native API only
const_rotator,4.4 Constants,function,NewBlueprint,F_const_rotator,rotator literal pin,UK2Node_CallFunction,rotator,Pitch=0; Yaw=90; Roll=0,lit(target.Input) P/Y/R,EX_RotationConst,§4.4,ok,none
const_transform,4.4 Constants,function,NewBlueprint,F_const_transform,transform literal pin,UK2Node_CallFunction,transform,identity,lit(target.Input)=identity,EX_TransformConst,§4.4,ok,LWC - element size is double on 5.5.4; reads with the package version into the transform pool
const_struct,4.4 Constants,function,NewBlueprint,F_const_struct,literal pin on a custom struct input,UK2Node_CallFunction,struct (S_ProbeStruct),member literals,lit each member pin,EX_StructConst;EX_EndStructConst,§4.4,ok,Term only - always lifted and rendered through the operand case as Name(...)
const_skipoffset,4.4 Constants,function,NewBlueprint,F_const_skipoffset,Delay,UK2Node_CallFunction,real,duration 1.0,exec entry -> Delay; lit(Duration)=1.0,EX_SkipOffsetConst,§4.4,nothing,Latent node - the token logs only and pushes no operand
setarray_makearray,4.5 Containers,function,NewBlueprint,F_setarray_makearray,Make Array,UK2Node_MakeArray,array<int32>,"1, 2",Make Array -> Set ArrayVar,EX_SetArray;EX_IntConst;EX_IntConst;EX_EndArray,§4.5,ok,"Statement form - renders as `TArray<int32>{1, 2}`"
setset_makeset,4.5 Containers,function,NewBlueprint,F_setset_makeset,Make Set,UK2Node_MakeSet,set<name>,Alpha; Beta (names),Make Set -> Set SetVar,EX_SetSet;EX_NameConst;EX_NameConst;EX_EndSet,§4.5,ok,"EX_SetSet has no input-side lift, so its operand position is unverified"
setmap_makemap,4.5 Containers,function,NewBlueprint,F_setmap_makemap,Make Map,UK2Node_MakeMap,"map<int32,name>","1:Alpha, 2:Beta",Make Map -> Set MapVar,EX_SetMap;EX_IntConst;EX_NameConst;EX_IntConst;EX_NameConst;EX_EndMap,§4.5,ok,"Key/value pins alternate; renders `TMap<int32, FName>{{1, ""Alpha""}, {2, ""Beta""}}`"
arrayconst_paramdefault,4.5 Containers,library_call,BFL_ProbeLibrary,F_arrayconst_paramdefault,call DefaultArray with the Ints pin DISCONNECTED,UK2Node_CallFunction,"TArray<int32>",parameter default [1;2;3],exec entry -> DefaultArray; LEAVE the Ints pin disconnected,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_LocalVariable;EX_Self;EX_EndFunctionParms,§4.5,expr-default,"The Ints pin carries no default value, so the call runs on the library CDO. A container pin default is not a usable route either: FNodeHandlingFunctor::RegisterLiteral drops it because the schema rejects container and reference pin defaults. structconst_containers exercises EX_ArrayConst"
setconst_paramdefault,4.5 Containers,library_call,BFL_ProbeLibrary,F_setconst_paramdefault,call DefaultSet with the Names pin DISCONNECTED,UK2Node_CallFunction,TSet<FName>,parameter default,exec entry -> DefaultSet; LEAVE the Names pin disconnected,EX_SetConst;EX_NameConst;EX_EndSetConst,§4.5,expr-default,Same split as arrayconst_paramdefault
mapconst_paramdefault,4.5 Containers,library_call,BFL_ProbeLibrary,F_mapconst_paramdefault,call DefaultMap with the Lookup pin DISCONNECTED,UK2Node_CallFunction,"TMap<int32,FName>",parameter default,exec entry -> DefaultMap; LEAVE the Lookup pin disconnected,EX_MapConst;EX_IntConst;EX_NameConst;EX_EndMapConst,§4.5,expr-default,The only way to characterise the key/value pairing loss - keys and values land flat in InputNodes
structconst_containers,4.5 Containers,library_call,BFL_ProbeLibrary,F_structconst_containers,call DefaultStruct with the Value pin DISCONNECTED,UK2Node_CallFunction,S_ProbeContainers,parameter default with container members,exec entry -> DefaultStruct; LEAVE the Value pin disconnected,EX_StructConst;EX_ArrayConst;EX_SetConst;EX_MapConst;EX_EndStructConst,§4.5,expr-default,Typed container literals nested inside a struct literal
cast_impure,4.6 Casts,function,NewBlueprint,F_cast_impure,Cast To BP_ProbeCastTarget (impure),UK2Node_DynamicCast,object ref,ActorVar as the Object input,exec entry -> Cast; failure path wired to a Set; data Cast.AsBP_ProbeCastTarget -> Set,EX_Let;EX_DynamicCast;EX_Let;EX_Cast;EX_JumpIfNot;EX_Jump,§4.6,ok,Best single cast fixture - six tokens in one chain; the EX_Cast carries CST_ObjectToBool
cast_pure,4.6 Casts,function,NewBlueprint,F_cast_pure,Cast To BP_ProbeCastTarget (pure),UK2Node_DynamicCast,object ref,ActorVar as the Object input,data Cast.As -> Set ObjectVar,EX_Let;EX_DynamicCast,§4.6,ok,A pure cast produces only the null-property Let plus the cast term
cast_metacast,4.6 Casts,function,NewBlueprint,F_cast_metacast,Cast To BP_ProbeCastTarget with a Class input,UK2Node_ClassDynamicCast,class ref,ClassVar as the Class input,data Cast.AsClass -> Set ClassVar,EX_Let;EX_MetaCast,§4.6,ok,Renders through the grouped cast case
cast_objecttointerface,4.6 Casts,function,NewBlueprint,F_cast_objecttointerface,Cast To BPI_ProbeA,UK2Node_DynamicCast,object ref,ActorVar as the Object input,data Cast.AsBPI_ProbeA -> Set InterfaceVar,EX_Let;EX_ObjToInterfaceCast,§4.6,ok,None as an operand; the statement path is fixture-only (R14)
cast_crossinterface,4.6 Casts,function,NewBlueprint,F_cast_crossinterface,Cast To BPI_ProbeB,UK2Node_DynamicCast,interface ref (BPI_ProbeA),InterfaceVar as the input,data Cast.AsBPI_ProbeB -> Set InterfaceVar,EX_CrossInterfaceCast,§4.6,ok,The input must already be an interface reference
cast_interface_to_object,4.6 Casts,function,NewBlueprint,F_cast_interface_to_object,Cast To BP_ProbeCastTarget,UK2Node_DynamicCast,interface ref (BPI_ProbeA),InterfaceVar as the input,data Cast.As -> Set ObjectVar,EX_InterfaceToObjCast,§4.6,ok,Renders through the grouped cast case
cast_float_double,4.6 Casts,native_only,NewBlueprint,,wire a native float output into a real pin,,float -> real,0.5,data GetSinglePrecision -> Set RealVar,EX_Cast;EX_Let,§4.6,ok,CST_FloatToDouble or CST_DoubleToFloat - needs a native float source
delegate_addelegate,4.7 Delegates,function,NewBlueprint,F_delegate_addelegate,Bind Event OnProbeMulticast,UK2Node_AddDelegate,multicast delegate,F_ProbeDelegateHandler,exec entry -> Bind Event,EX_AddMulticastDelegate,§4.7,ok,Regression guard
delegate_assigndelegateset,4.7 Delegates,function,NewBlueprint,F_delegate_assigndelegateset,Assign OnProbeMulticast,UK2Node_DelegateSet,multicast delegate,Create Event -> F_ProbeDelegateHandler,exec entry -> Assign,EX_AddMulticastDelegate,§4.7,ok,"UK2Node_DelegateSet is deprecated and ubergraph-only. Against a BP dispatcher it cannot resolve its signature and the compiler asserts in PerformFixups, so this row stays absent; delegate_addelegate covers EX_AddMulticastDelegate"
delegate_removedelegate,4.7 Delegates,function,NewBlueprint,F_delegate_removedelegate,Unbind Event OnProbeMulticast,UK2Node_RemoveDelegate,multicast delegate,F_ProbeDelegateHandler,exec entry -> Unbind Event,EX_RemoveMulticastDelegate,§4.7,ok,none
delegate_cleardelegate,4.7 Delegates,function,NewBlueprint,F_delegate_cleardelegate,Unbind all Events OnProbeMulticast,UK2Node_ClearDelegate,multicast delegate,None,exec entry -> Unbind all Events,EX_ClearMulticastDelegate,§4.7,ok,none
delegate_createdelegate,4.7 Delegates,function,NewBlueprint,F_delegate_createdelegate,Create Event -> F_ProbeDelegateHandler,UK2Node_CreateDelegate,none,F_ProbeDelegateHandler,data Create Event -> Set SingleDelegateVar,EX_BindDelegate,§4.7,ok,The decoder reads the function name first and pushes it last so it renders as the trailing argument
delegate_callmulticast,4.7 Delegates,function,NewBlueprint,F_delegate_callmulticast,Call OnProbeMulticast,UK2Node_CallDelegate,multicast delegate,Value=7,exec entry -> Call; lit(Value)=7,EX_CallMulticastDelegate;EX_IntConst;EX_EndFunctionParms,§4.2,ok,none
delegate_instance,4.7 Delegates,library_call,BFL_ProbeLibrary,F_delegate_instance,call SingleDelegate with the Del pin DISCONNECTED,UK2Node_CallFunction,single-cast delegate,delegate-typed parameter default,exec entry -> SingleDelegate; LEAVE the Del pin disconnected,EX_InstanceDelegate,§4.7,quoted-name,"SingleDelegate now exists on BFL_ProbeLibrary with a delegate parameter; the disconnected Del pin emits EX_InstanceDelegate, and the pseudo-source renders the typed FScriptDelegate literal"
flow_branch,4.8 Control flow,function,NewBlueprint,F_flow_branch,Branch on BoolVar,UK2Node_IfThenElse,bool,BoolVar set from a member get,exec entry -> Branch; True -> Set IntVar; False -> Set IntVar,EX_JumpIfNot;EX_Jump,§4.8,ok,Both arms run when BoolVar is driven; the runtime step test runs each arm
flow_while,4.8 Control flow,function,NewBlueprint,F_flow_while,Branch header on a computed comparison,UK2Node_IfThenElse,bool,IntVar < 3 stored in a temp before the branch,exec entry -> Set IntVar=0 -> Less(IntVar; 3) -> Branch; True -> Add IntVar; Set IntVar -> Less; False -> return,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Return;EX_Nothing;EX_Let;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Jump,§4.8,ok,The condition statement is the loop header; the structurer lifts it into while (Less(...)) and the branch owns no line
flow_do_while,4.8 Control flow,function,NewBlueprint,F_flow_do_while,Branch at the tail wired back to the body,UK2Node_IfThenElse,bool,BoolVar set from a member get,exec entry -> Set IntVar=0 -> Add IntVar; Set IntVar; Set BoolVar=(IntVar<3) -> Branch; True -> body start; False -> return,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Let;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_LetBool;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_InstanceVariable;EX_Return;EX_Nothing;EX_Jump,§4.8,ok,The body runs before the condition and exits after three iterations
flow_break,4.8 Control flow,function,NewBlueprint,F_flow_break,Branch inside the loop with a break arm,UK2Node_IfThenElse,bool,BoolVar set from a member get,exec entry -> Set IntVar=0 -> Set BoolVar=true -> Branch; True -> Add IntVar; Set IntVar -> Branch(IntVar==2); True -> break; False -> Set BoolVar=(IntVar<3) -> header; False -> return,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_InstanceVariable;EX_True;EX_WireTracepoint;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_InstanceVariable;EX_Return;EX_Nothing;EX_Let;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_LetBool;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Jump;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript,§4.8,ok,The break arm leaves the loop on the second iteration and the false arm updates the condition
flow_continue,4.8 Control flow,function,NewBlueprint,F_flow_continue,Branch inside the loop wired back to the header,UK2Node_IfThenElse,bool,BoolVar set from a member get,exec entry -> Set IntVar=0 -> Set BoolVar=true -> Branch; True -> Add IntVar; Set IntVar -> Branch(IntVar==1); True -> header; False -> Set BoolVar=(IntVar<3) -> header; False -> return,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_InstanceVariable;EX_True;EX_WireTracepoint;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_InstanceVariable;EX_Return;EX_Nothing;EX_Let;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_LetBool;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Jump;EX_Jump,§4.8,ok,continue jumps to the header on the first iteration and the false arm updates the condition
flow_nested_loops,4.8 Control flow,function,NewBlueprint,F_flow_nested_loops,Outer while on LoopFlag whose body is a while on BoolVar with an inner continue and break,UK2Node_IfThenElse,bool,LoopFlag and BoolVar set from member gets,exec entry -> Set IntVar=0 -> Set LoopFlag=true -> Outer Branch; True -> Set BoolVar=true -> Inner Branch; True -> Add IntVar; Set IntVar -> Branch(IntVar==1); True -> continue; False -> Branch(IntVar==2); True -> break; False -> inner header; Inner False -> Set LoopFlag=false -> outer header; Outer False -> return,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_InstanceVariable;EX_True;EX_WireTracepoint;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_InstanceVariable;EX_Return;EX_Nothing;EX_LetBool;EX_InstanceVariable;EX_True;EX_WireTracepoint;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_InstanceVariable;EX_LetBool;EX_InstanceVariable;EX_False;EX_WireTracepoint;EX_Jump;EX_Let;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_Tracepoint;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Jump;EX_LetBool;EX_InstanceVariable;EX_False;EX_WireTracepoint;EX_Jump;EX_Jump,§4.8,ok,The inner break and continue resolve against the innermost loop; LoopFlag ends the outer loop after one pass
flow_sequence,4.8 Control flow,function,NewBlueprint,F_flow_sequence,Sequence with both outputs wired,UK2Node_ExecutionSequence,none,None,exec entry -> Sequence; Then0 -> Set IntVar; Then1 -> Set IntVar,EX_PushExecutionFlow;EX_PopExecutionFlow,§4.8,ok,PushExecutionFlow emits a breakpointable boundary comment; PopExecutionFlow is dropped before the switch
flow_switch,4.8 Control flow,function,NewBlueprint,F_flow_switch,Switch on Int with two cases and a default,UK2Node_SwitchInteger,int,IntVar selects,exec entry -> Switch; Case 0 -> Set RealVar; Case 1 -> Set RealVar; Default -> Set RealVar,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_DoubleConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_DoubleConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_Let;EX_InstanceVariable;EX_DoubleConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript,§4.8,ok,Each case is a compare plus a JumpIfNot so every case body is its own statement and stop point
flow_computedjump,4.8 Control flow,event_ubergraph,NewBlueprint,EventGraph,any Event,UK2Node_Event,none,None,exec entry -> any node,EX_ComputedJump,n/a,ok,"Captured from ExecuteUbergraph_BP_OpcodeDemo_All; the jump table at the head of the stub is the only route to this token"
flow_popexecutionflowifnot,4.8 Control flow,function,NewBlueprint,F_flow_popexecutionflowifnot,Branch at the end of a Sequence with no code after one path,UK2Node_IfThenElse,bool,lit(Condition)=true,exec entry -> Sequence -> Branch,EX_PopExecutionFlowIfNot,§4.8,ok,Compiler-generated when a conditional goto's target is unresolved - layout-dependent
flow_switchvalue,4.8 Control flow,function,NewBlueprint,F_flow_switchvalue,Select,UK2Node_Select,index int; outputs int,"Index 0,1",data IndexVar -> Index; data A and B -> Select outputs,EX_SwitchValue,§4.8,ok,Always an inline operand attached to the Select's return term
flow_arraygetbyref,4.8 Control flow,function,NewBlueprint,F_flow_arraygetbyref,GET off an array pin,UK2Node_GetArrayItem,array<int32> + int index,Index=1,data ArrayVar -> Array; lit(Index)=1; data GET -> Set IntVar,EX_ArrayGetByRef,§4.8,ok,No node is created - array and index become ordinary operands and ReturnNode points at the array's Inner property
flow_return_value,4.8 Control flow,function,NewBlueprint,F_flow_return_value,a fixture function with an int return value,UK2Node_FunctionResult,int,7,Return Node value = 7,EX_Return;EX_LocalOutVariable,§4.8,ok,The valued-return shape; its terminator differs from the void form
flow_return_void,4.8 Control flow,function,NewBlueprint,F_flow_return_void,a fixture function with no return value,UK2Node_FunctionResult,none,None,implicit Return Node,EX_Return;EX_Nothing,§4.8,ok,The visualizer trims this trailing bare return when it is the last top-level line
debug_tracepoint,4.9 Debug,build_setting,NewBlueprint,,any exec node,UK2Node_CallFunction,none,None,any exec chain,EX_Tracepoint,§4.9,nothing,Editor-only - absent from the headless commandlet run; observe interactively (R15)
debug_wiretracepoint,4.9 Debug,build_setting,NewBlueprint,,KCST_WireTraceSite,UK2Node_CallFunction,none,None,any wire,EX_WireTracepoint,§4.9,nothing,Build setting rather than a node choice (R15)
debug_breakpoint,4.9 Debug,build_setting,NewBlueprint,,place a breakpoint on a node at debug time,,none,None,any exec node,EX_Breakpoint,§4.9,nothing,Patched into a live script by the editor and rewritten to EX_Tracepoint on load
debug_instrumentation_inlineevent,4.9 Debug,build_setting,NewBlueprint,,instrumentation build (BP profiler),,none,None,any exec node,EX_InstrumentationEvent,§4.9,nothing,"No stock producer; the decoder reads the event byte and the 12-byte name for InlineEvent"
terminators,4.10 Terminators,function,NewBlueprint,F_terminators,leave the function empty,UK2Node_FunctionResult,none,None,an empty function body - nothing to wire,EX_EndOfScript;EX_EndFunctionParms;EX_EndArray;EX_EndSet;EX_EndMap;EX_EndStructConst;EX_EndArrayConst;EX_EndSetConst;EX_EndMapConst,n/a,n/a,Pure terminators with no payload and no node - nothing to lay out and nothing to assert beyond the cursor
```

## 4. Per-engine overrides

The `opcodes` column describes the reference engine (UE 5.8). When another engine's compiler emits a
different sequence for a row, restate that row here instead of weakening the shared plan. The token
test resolves a row by exact `<major>.<minor>` match, then falls back to the CSV:

```csv
fixture_id,engine,opcodes
# fixture_id,<major>.<minor>,EX_Token;EX_Token
arrayconst_paramdefault,5.8,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_LocalVariable;EX_EndFunctionParms
arrayconst_paramdefault,5.7,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_LocalVariable;EX_EndFunctionParms
arrayconst_paramdefault,5.6,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_LocalVariable;EX_EndFunctionParms
arrayconst_paramdefault,5.5,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_LocalVariable;EX_EndFunctionParms
# 5.3 and 5.4 evaluate the object operand of a library call before the parameter operand, like 5.5+.
arrayconst_paramdefault,5.4,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_LocalVariable;EX_EndFunctionParms
arrayconst_paramdefault,5.3,EX_Context;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_LocalVariable;EX_EndFunctionParms
# 4.25 has no double pins: real values are float, so the DoubleConst rows emit FloatConst, and the
# primitive cast token is EX_PrimitiveCast rather than EX_Cast.
let_real,4.25,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_EndOfScript
const_double,4.25,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Context_FailSilent;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_FloatConst;EX_EndFunctionParms;EX_WireTracepoint;EX_Return;EX_Nothing;EX_EndOfScript
cast_impure,4.25,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_LocalVariable;EX_DynamicCast;EX_InstanceVariable;EX_Let;EX_LocalVariable;EX_PrimitiveCast;EX_LocalVariable;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_LetObj;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript
flow_switch,4.25,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript
# 4.26 and 4.27 are the same UE4 compiler family as 4.25.
let_real,4.26,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_EndOfScript
const_double,4.26,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Context_FailSilent;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_FloatConst;EX_EndFunctionParms;EX_WireTracepoint;EX_Return;EX_Nothing;EX_EndOfScript
cast_impure,4.26,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_LocalVariable;EX_DynamicCast;EX_InstanceVariable;EX_Let;EX_LocalVariable;EX_PrimitiveCast;EX_LocalVariable;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_LetObj;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript
flow_switch,4.26,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript
let_real,4.27,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_EndOfScript
const_double,4.27,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Context_FailSilent;EX_ObjectConst;EX_LocalVirtualFunction;EX_Self;EX_FloatConst;EX_EndFunctionParms;EX_WireTracepoint;EX_Return;EX_Nothing;EX_EndOfScript
cast_impure,4.27,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_Let;EX_LocalVariable;EX_DynamicCast;EX_InstanceVariable;EX_Let;EX_LocalVariable;EX_PrimitiveCast;EX_LocalVariable;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_IntConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_LetObj;EX_InstanceVariable;EX_LocalVariable;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript
flow_switch,4.27,EX_Tracepoint;EX_WireTracepoint;EX_Tracepoint;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_LetBool;EX_LocalVariable;EX_CallMath;EX_InstanceVariable;EX_IntConst;EX_EndFunctionParms;EX_WireTracepoint;EX_JumpIfNot;EX_LocalVariable;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Return;EX_Nothing;EX_Let;EX_InstanceVariable;EX_FloatConst;EX_WireTracepoint;EX_Jump;EX_Return;EX_Nothing;EX_EndOfScript
```

Rules:

- One row per fixture and engine. A later row for the same pair replaces an earlier one.
- `opcodes` is the complete replacement for the CSV row, not a delta.
- An override only applies when its `engine` equals the running engine's
  `ENGINE_MAJOR_VERSION.ENGINE_MINOR_VERSION`. A non-matching engine keeps the CSV row.
- A row must name a fixture the CSV defines. `BlueprintInspector.Fixture.PlanOverridesResolve` fails on
  a typo.

UE 4.25, 4.26, 4.27, 5.0, 5.1, 5.2, 5.5, 5.6 and 5.7 keep their own golden variants (`PseudoSource.<engine>.golden`,
`PseudoSource.map.<engine>.golden`) because their text differs from the 5.8 reference. 4.x and 5.0-5.2 emit
`EX_FloatConst`/`EX_PrimitiveCast`, empty struct defaults and lack the delegate and array-const fixtures.
5.5-5.7 include the transient select row, and their variants capture the remaining engine differences. 5.3 and
5.4 need no variant. Their text matches the shared golden, and only `arrayconst_paramdefault` needs an
override row. The golden test uses the variant when the file exists.

## 5. Fitting the layout to the automation tests

The tests are in `Private/Tests/BlueprintInspectorAutomationTests.cpp`. They merge
`BP_OpcodeDemo_All`, `BP_OpcodeDemo_Structs` and the saved select asset through
`BlueprintFixtureSource`, assert token sequences through `FBlueprintOpcodeLogger::SetOpcodeSink`, and
handle the EventGraph rows separately: `let_persistent_frame` is captured from the `E_WithDataOut`
stub, `flow_computedjump` from `ExecuteUbergraph_BP_OpcodeDemo_All`. Every capture ends on the
script's `EX_EndOfScript`, so a decoded function covers its whole body.

1. **Walk every bytecode-bearing function across the merged fixture sources**, so adding a `F_*` graph
   needs no test edit.
2. **Handle the Event Graph separately.** EventGraph rows have no function graph: capture
   `let_persistent_frame` from the data-output event stub and `flow_computedjump` from the generated
   `ExecuteUbergraph_*` function. The golden and text tests still filter the ubergraph.
3. **Do not walk the parent class.** `ExcludeSuper` correctly limits the walk to the child's
   override, but a fixture placed on `BP_ProbeParent` is silently skipped.
4. **Assert the token sequence.** A check that only verifies "produced output" passes a graph that
   emits the wrong token. The decoded `FBlueprintNode` graph does not preserve operand tokens, so
   recover the sequence from `FBlueprintOpcodeLogger` through a per-function capture sink (or a
   scoped `GLog` hook) and compare it to the CSV's `opcodes` column.

## 6. Rows that are not plain regression guards

| Rows | `render` | Gap |
|---|---|---|
| `arrayconst_paramdefault`, `setconst_paramdefault`, `mapconst_paramdefault`, `structconst_containers` | `expr-default` | Container literals render as typed `TArray`/`TSet`/`TMap` initializers. `EX_MapConst` pairs keys and values |
| `cast_float_double` | `ok` | Unlisted `CST_*` render `Cast<0x…>(…)` |
| `const_softobject`, `const_fieldpath`, `delegate_instance` | `quoted-name` | `delegate_instance` renders a typed `FScriptDelegate`. The others are bare quoted `FName` |
| `flow_arraygetbyref` | `ok` | No `Array[Index]` form |
| `context_output_lvalue` | `ok` | Context-shaped l-value sets `ReturnNode`. Verify the rendered name in a run |
| debug rows | `nothing` | No stock producer. The decoder reads the `InlineEvent` payload |
| `const_skipoffset` | `nothing` | Logs only, pushes nothing |

This document only says which fixture pins each gap. It does not track corpus status.

## 7. What this document does not cover

- **Byte recipes.** Out of scope. This document stops at the graph layout.
- **Tokens no frontend emits.** No node can reach them, so there is no layout.
- **Statement-position casts, `EX_SwitchValue` and `EX_ArrayGetByRef`.** Not layable (R14).
- **A blueprint-scripting tool.** This is the data a scripter would consume, not the scripter.
