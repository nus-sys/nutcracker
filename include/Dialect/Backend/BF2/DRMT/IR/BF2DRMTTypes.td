//===- BF2DRMTTypes.td - BF3 NIC ASIC DRMT dialect types -*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef BF2DRMT_TYPES
#define BF2DRMT_TYPES

include "Dialect/Backend/BF2/DRMT/IR/BF2DRMTDialect.td"
include "mlir/IR/AttrTypeBase.td"

class BF2DRMT_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<BF2DRMT_Dialect, name, traits> {
    let mnemonic = typeMnemonic;
}

//===----------------------------------------------------------------------===//
// ReferenceType
//===----------------------------------------------------------------------===//

def BF2DRMT_ReferenceType : BF2DRMT_Type<"Reference", "ref"> {
    let summary = "reference to a bf2drmt-typed value";
    let description = [{ A !bf2drmt.ref<T> holds the address of a T. }];
    let parameters = (ins "mlir::Type":$objectType);
    let builders = [
        TypeBuilderWithInferredContext<(ins "mlir::Type":$objectType), [{
            return $_get(objectType.getContext(), objectType);
        }]>,
        TypeBuilder<(ins "mlir::Type":$objectType), [{
            return $_get($_ctxt, objectType);
        }]>
    ];
    let skipDefaultBuilders = 1;
    let assemblyFormat = [{ `<` $objectType `>` }];
}

//===----------------------------------------------------------------------===//
// StructType
//===----------------------------------------------------------------------===//

def BF2DRMT_StructType : BF2DRMT_Type<"Struct", "struct"> {
    let summary = "bf2drmt struct type (named aggregate)";
    let hasCustomAssemblyFormat = 1;
    let genVerifyDecl = 1;
    let parameters = (
        ins StringRefParameter<"struct name">:$name,
            ArrayRefParameter<"::mlir::vdrmt::FieldInfo", "struct fields">:$elements,
            OptionalParameter<"mlir::DictionaryAttr", "annotations">:$annotations
    );
    let skipDefaultBuilders = 1;
    let builders = [
        TypeBuilder<(ins "llvm::StringRef":$name,
                         "llvm::ArrayRef<::mlir::vdrmt::FieldInfo>":$fields,
                         CArg<"mlir::DictionaryAttr", "{}">:$annotations), [{
            return $_get($_ctxt, name, fields,
                         annotations && !annotations.empty() ? annotations
                                                              : mlir::DictionaryAttr());
        }]>
    ];
}

//===----------------------------------------------------------------------===//
// HeaderType
//===----------------------------------------------------------------------===//

def BF2DRMT_HeaderType : BF2DRMT_Type<"Header", "header"> {
    let summary = "bf2drmt header type (named aggregate with validity bit)";
    let hasCustomAssemblyFormat = 1;
    let genVerifyDecl = 1;
    let parameters = (
        ins StringRefParameter<"header name">:$name,
            ArrayRefParameter<"::mlir::vdrmt::FieldInfo", "header fields">:$elements,
            OptionalParameter<"mlir::DictionaryAttr", "annotations">:$annotations
    );
    let skipDefaultBuilders = 1;
    let builders = [
        TypeBuilder<(ins "llvm::StringRef":$name,
                         "llvm::ArrayRef<::mlir::vdrmt::FieldInfo>":$fields,
                         CArg<"mlir::DictionaryAttr", "{}">:$annotations), [{
            return $_get($_ctxt, name, fields,
                         annotations && !annotations.empty() ? annotations
                                                              : mlir::DictionaryAttr());
        }]>
    ];
}

#endif // BF2DRMT_TYPES
