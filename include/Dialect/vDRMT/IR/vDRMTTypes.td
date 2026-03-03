//===- vDRMTTypes.td - vDRMT dialect types -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDRMTTYPES
#define VDRMTTYPES

include "Dialect/vDRMT/IR/vDRMTDialect.td"
include "Dialect/vDRMT/IR/vDRMTTypeInterfaces.td"

//===----------------------------------------------------------------------===//
// vDRMT type definitions.
//===----------------------------------------------------------------------===//

class vDRMT_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<vDRMT_Dialect, name, traits> {
    let mnemonic = typeMnemonic;
}

//===----------------------------------------------------------------------===//
// VoidType
//===----------------------------------------------------------------------===//

def VoidType : vDRMT_Type<"Void", "void"> {
  let summary = "void type";
  let description = [{
    Represents absense of result of actions and methods, or `void` type for functions.
  }];
  let extraClassDeclaration = [{
    llvm::StringRef getAlias() const { return "void"; };
  }];
}

//===----------------------------------------------------------------------===//
// FuncType
//===----------------------------------------------------------------------===//

def FuncType : vDRMT_Type<"Func", "func"> {
    let summary = "function-like type (actions, methods, functions)";

    let parameters = (ins ArrayRefParameter<"mlir::Type">:$inputs,
                            "mlir::Type":$optionalReturnType,
                            OptionalArrayRefParameter<"mlir::Type">:$type_arguments);

    let builders = [
        // Construct with an actual return type or explicit !p4hir.void
        TypeBuilderWithInferredContext<(ins
        "llvm::ArrayRef<mlir::Type>":$inputs, "mlir::Type":$returnType), [{
        return $_get(returnType.getContext(), inputs,
                    mlir::isa<vdrmt::VoidType>(returnType) ? nullptr
                                                            : returnType,
                    llvm::ArrayRef<mlir::Type>());
        }]>,

        // Construct with an actual return type or explicit !p4hir.void
        TypeBuilderWithInferredContext<(ins
        "llvm::ArrayRef<mlir::Type>":$inputs, "mlir::Type":$returnType, "llvm::ArrayRef<mlir::Type>":$typeArgs), [{
        return $_get(returnType.getContext(), inputs,
                    mlir::isa<vdrmt::VoidType>(returnType) ? nullptr
                                                            : returnType,
                    typeArgs);
        }]>,

        // Construct without return type
        TypeBuilder<(ins "llvm::ArrayRef<mlir::Type>":$inputs), [{
        return $_get($_ctxt, inputs, nullptr, llvm::ArrayRef<mlir::Type>());
        }]>

    ];

    // Use a custom parser to handle the argument types in better way.
    let assemblyFormat = [{
        `<` (`<` $type_arguments^ `>`)? custom<FuncType>($inputs, $optionalReturnType) `>`
    }];

    let extraClassDeclaration = [{
        /// Returns the `i`th input operand type. Asserts if out of bounds.
        mlir::Type getInput(unsigned i) const { return getInputs()[i]; }

        /// Returns the number of arguments to the function.
        unsigned getNumInputs() const { return getInputs().size(); }

        /// Returns the result type of the function as an actual return type or explicit void
        mlir::Type getReturnType() const;

        /// Returns the result type of the function as an ArrayRef, enabling better
        /// integration with generic MLIR utilities.
        llvm::ArrayRef<mlir::Type> getReturnTypes() const;

        /// Returns a clone of this action type with the given argument
        /// and result types. Required for FunctionOp interface
        FuncType clone(mlir::TypeRange inputs, mlir::TypeRange outputs) const;

        /// Returns whether the function returns void.
        bool isVoid() const;
    }];
}

//===----------------------------------------------------------------------===//
// StructType
//===----------------------------------------------------------------------===//

class vDRMT_StructLikeType<string name, string typeMnemonic>
  : vDRMT_Type<name, typeMnemonic,  [
    vDRMT_StructLikeTypeInterface,
    DestructurableTypeInterface,
    vDRMT_FieldIDTypeInterface
]> {
  let hasCustomAssemblyFormat = 1;
  let genVerifyDecl = 1;

  let parameters = (
    ins StringRefParameter<"struct name">:$name,
        ArrayRefParameter<"vdrmt::FieldInfo", "struct fields">:$elements,
        OptionalParameter<"mlir::DictionaryAttr", "annotations">:$annotations
  );

  let skipDefaultBuilders = 1;
  let builders = [
    TypeBuilder<(ins "llvm::StringRef":$name,
                     "llvm::ArrayRef<vdrmt::FieldInfo>":$fields,
                     CArg<"mlir::DictionaryAttr", "{}">:$annotations), [{
      return $_get($_ctxt, name, fields,
                   annotations && !annotations.empty() ? annotations : mlir::DictionaryAttr());
    }]>
  ];

  string extra2ClassDeclaration = "";
  let extraClassDeclaration = !strconcat([{
    mlir::Type getFieldType(mlir::StringRef fieldName) {
      for (const auto &field : getElements())
        if (field.name == fieldName) return field.type;
       return {};
    }

    std::optional<vdrmt::FieldInfo> getField(mlir::StringRef fieldName) {
      for (const auto &field : getElements())
        if (field.name == fieldName) return field;
       return {};
    }

    llvm::ArrayRef<vdrmt::FieldInfo> getFields() {
      return getElements();
    }

    void getInnerTypes(mlir::SmallVectorImpl<mlir::Type> &types) {
      for (const auto &field : getElements()) types.push_back(field.type);
    }

    std::optional<unsigned> getFieldIndex(mlir::StringRef fieldName) {
      llvm::ArrayRef<vdrmt::FieldInfo> elems = getElements();
      for (size_t idx = 0, numElems = elems.size(); idx < numElems; ++idx)
        if (elems[idx].name == fieldName) return idx;
      return {};
    }

    std::optional<unsigned> getFieldIndex(mlir::StringAttr fieldName) {
      llvm::ArrayRef<vdrmt::FieldInfo> elems = getElements();
      for (size_t idx = 0, numElems = elems.size(); idx < numElems; ++idx)
        if (elems[idx].name == fieldName) return idx;
      return {};
    }

    // FieldID type interface implementation
    std::pair<unsigned, llvm::SmallVector<unsigned>> getFieldIDsStruct() const {
      unsigned fieldID = 0;
      auto elements = getElements();
      llvm::SmallVector<unsigned> fieldIDs;
      fieldIDs.reserve(elements.size());
      for (auto &element : elements) {
          auto type = element.type;
          fieldID += 1;
          fieldIDs.push_back(fieldID);
          // Increment the field ID for the next field by the number of subfields.
          fieldID += FieldIdImpl::getMaxFieldID(type);
      }
      return {fieldID, fieldIDs};
    }

    std::pair<mlir::Type, unsigned> getSubTypeByFieldID(unsigned fieldID) const {
      if (fieldID == 0) return {*this, 0};
      auto [maxId, fieldIDs] = getFieldIDsStruct();
      auto *it = std::prev(llvm::upper_bound(fieldIDs, fieldID));
      auto subfieldIndex = std::distance(fieldIDs.begin(), it);
      auto subfieldType = getElements()[subfieldIndex].type;
      auto subfieldID = fieldID - fieldIDs[subfieldIndex];
      return {subfieldType, subfieldID};
    }

    mlir::Type getTypeAtIndex(mlir::Attribute index) const {
      auto indexAttr = mlir::dyn_cast<mlir::IntegerAttr>(index);
      if (!indexAttr) return {};

      return getSubTypeByFieldID(indexAttr.getInt()).first;
    }

    unsigned getFieldID(unsigned index) const {
      auto [maxId, fieldIDs] = getFieldIDsStruct();
      return fieldIDs[index];
    }

    unsigned getMaxFieldID() const {
      unsigned fieldID = 0;
      for (const auto &field : getElements()) fieldID += 1 + FieldIdImpl::getMaxFieldID(field.type);
      return fieldID;
    }

    unsigned getIndexForFieldID(unsigned fieldID) const {
      assert(!getElements().empty() && "struct must have >0 fields");
      auto [maxId, fieldIDs] = getFieldIDsStruct();
      auto *it = std::prev(llvm::upper_bound(fieldIDs, fieldID));
      return std::distance(fieldIDs.begin(), it);
    }

    std::pair<unsigned, unsigned> getIndexAndSubfieldID(unsigned fieldID) const {
      auto index = getIndexForFieldID(fieldID);
      auto elementFieldID = getFieldID(index);
      return {index, fieldID - elementFieldID};
    }

    std::pair<unsigned, bool> projectToChildFieldID(unsigned fieldID,
                                                    unsigned index) const {
      auto [maxId, fieldIDs] = getFieldIDsStruct();
      auto childRoot = fieldIDs[index];
      auto rangeEnd = index + 1 >= getElements().size() ? maxId : (fieldIDs[index + 1] - 1);
      return std::make_pair(fieldID - childRoot, fieldID >= childRoot && fieldID <= rangeEnd);
    }

    std::optional<llvm::DenseMap<mlir::Attribute, mlir::Type>> getSubelementIndexMap() const {
      llvm::DenseMap<mlir::Attribute, mlir::Type> destructured;
      for (auto [i, field] : llvm::enumerate(getElements()))
          destructured.try_emplace(mlir::IntegerAttr::get(mlir::IndexType::get(getContext()), i), field.type);
      return destructured;
    }
  }], "\n", extra2ClassDeclaration);
}

def StructType : vDRMT_StructLikeType<"Struct", "struct"> {
    let summary = "struct type";
    let description = [{
        Represents a structure of name, value pairs.
        !vdrmt.struct<"name", fieldName1: Type1, fieldName2: Type2>
    }];
}

//===----------------------------------------------------------------------===//
// HeaderType
//===----------------------------------------------------------------------===//

def HeaderType : vDRMT_StructLikeType<"Header", "header"> {
 let summary = "header type";
 let description = [{
   Represents a structure of name, value pairs.
   !vdrmt.header<"name", fieldName1: Type1, fieldName2: Type2>
 }];
}

//===----------------------------------------------------------------------===//
// ReferenceType
//===----------------------------------------------------------------------===//

def ReferenceType : vDRMT_Type<"Reference", "ref"> {
    let summary = "reference type";
    let description = [{
        `p4hir.ref` is a type returned by any op referring to an object in P4.
    }];

    let parameters = (ins "mlir::Type":$objectType);

    let builders = [
        TypeBuilderWithInferredContext<(ins "mlir::Type":$objectType), [{
            return $_get(objectType.getContext(), objectType);
        }]>,
        TypeBuilder<(ins "mlir::Type":$objectType), [{
            return $_get($_ctxt, objectType);
        }]>
    ];

    let assemblyFormat = [{
        `<` $objectType `>`
    }];

    let skipDefaultBuilders = 1;
}

def AnyVDRMTType : AnyTypeOf<[StructType, HeaderType]> {}
def LoadableVDRMTType : AnyTypeOf<[StructType, HeaderType, AnyInteger]> {}

def StructLikeType : AnyTypeOf<[StructType, HeaderType]>;

/// A ref type with the specified constraints on the nested type.
class vDRMT_SpecificRefType<Type type> : ConfinedType<ReferenceType,
    [SubstLeaves<"$_self", "mlir::cast<vdrmt::ReferenceType>($_self).getObjectType()", type.predicate>],
    "ref of " # type.summary, "vdrmt::ReferenceType"
> {
    Type objectType = type;
}

def StructLikeRefType : vDRMT_SpecificRefType<StructLikeType>;

#endif // VDRMTTYPES