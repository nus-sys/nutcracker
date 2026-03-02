//===- vDRMTAttr.td - vDRMT dialect attributes -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDRMTATTRS
#define VDRMTATTRS

include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/IR/EnumAttr.td"

include "Dialect/vDRMT/IR/vDRMTDialect.td"

class vDRMT_Attr<string name, string attrMnemonic, list<Trait> traits = []>
    : AttrDef<vDRMT_Dialect, name, traits> {
  let mnemonic = attrMnemonic;
}

//===----------------------------------------------------------------------===//
// MatchKindAttr
//===----------------------------------------------------------------------===//
def vDRMT_MatchKindAttr : vDRMT_Dialect<"MatchKind", "match_kind",
  [
   // TypedAttrInterface
   ]> {
  let summary = "Represent match kind";
  let description = [{
    The MatchKindAttr represents a match kind value
  }];

  let parameters = (ins "mlir::StringAttr":$value);

  let assemblyFormat = [{
    `<` $value `>`
  }];

  let builders = [
    AttrBuilder<(ins "mlir::StringRef":$value), [{
      return $_get($_ctxt, mlir::StringAttr::get($_ctxt, value));
    }]>
  ];

}