//===- vDPPTypes.td - vDPP dialect types -----------*- tablegen -*-===//
//===----------------------------------------------------------------------===//

#ifndef VDPPTYPES
#define VDPPTYPES

//===----------------------------------------------------------------------===//
// vDPP type definitions.
//===----------------------------------------------------------------------===//

class vDPP_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<vDPP_Dialect, name, traits> {
    let mnemonic = typeMnemonic;
}

//===----------------------------------------------------------------------===//
// VoidType
//===----------------------------------------------------------------------===//

def VoidType : vDPP_Type<"Void", "void"> {
  let summary = "void type";
  let description = [{
    Represents absense of result of actions and methods, or `void` type for functions.
  }];
  let extraClassDeclaration = [{
    llvm::StringRef getAlias() const { return "void"; };
  }];
}

//===----------------------------------------------------------------------===//
// PointerType (similar to LLVM)
//===----------------------------------------------------------------------===//

def PointerType : vDPP_Type<"Pointer", "ptr"> {
  let summary = "vDPP pointer type";
  let description = [{
    Represents a pointer to an element type, similar to LLVM's pointer type.
    
    Unlike traditional typed pointers, this uses opaque pointers (like LLVM 15+).
    The element type is still tracked for type safety during lowering.
    
    Example:
    ```mlir
    !vdpp.ptr<i32>        // pointer to i32
    !vdpp.ptr<i8>         // pointer to i8
    !vdpp.ptr<!vdpp.struct<"foo", ...>>  // pointer to struct
    ```
  }];
  
  let parameters = (ins "mlir::Type":$elementType);
  
  let builders = [
    TypeBuilderWithInferredContext<(ins "mlir::Type":$elementType), [{
      return $_get(elementType.getContext(), elementType);
    }]>
  ];
  
  let assemblyFormat = "`<` $elementType `>`";
  
  let extraClassDeclaration = [{
    /// Check if this is a pointer to an integer type
    bool isIntegerPtr() const {
      return mlir::isa<mlir::IntegerType>(getElementType());
    }
    
    /// Check if this is a pointer to a struct type
    bool isStructPtr() const {
      return mlir::isa<mlir::vdpp::StructType>(getElementType());
    }
  }];
}

//===----------------------------------------------------------------------===//
// FuncType
//===----------------------------------------------------------------------===//

def FuncType : vDPP_Type<"Func", "func"> {
    let summary = "function-like type (actions, methods, functions)";

    let parameters = (ins ArrayRefParameter<"mlir::Type">:$inputs,
                            "mlir::Type":$optionalReturnType,
                            OptionalArrayRefParameter<"mlir::Type">:$type_arguments);

    let builders = [
        // Construct with an actual return type or explicit !p4hir.void
        TypeBuilderWithInferredContext<(ins
        "llvm::ArrayRef<mlir::Type>":$inputs, "mlir::Type":$returnType), [{
        return $_get(returnType.getContext(), inputs,
                    mlir::isa<vdpp::VoidType>(returnType) ? nullptr
                                                            : returnType,
                    llvm::ArrayRef<mlir::Type>());
        }]>,

        // Construct with an actual return type or explicit !p4hir.void
        TypeBuilderWithInferredContext<(ins
        "llvm::ArrayRef<mlir::Type>":$inputs, "mlir::Type":$returnType, "llvm::ArrayRef<mlir::Type>":$typeArgs), [{
        return $_get(returnType.getContext(), inputs,
                    mlir::isa<vdpp::VoidType>(returnType) ? nullptr
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
// StructType (LLVM-style)
//===----------------------------------------------------------------------===//

def StructType : vDPP_Type<"Struct", "struct"> {
  let summary = "vDPP structure type";
  let description = [{
    Represents a structure with fields, similar to LLVM's struct type.
    Can be named (identified) or literal (unnamed).
    
    Named struct:
    ```mlir
    !vdpp.struct<"mystruct", (i32, i64, i8)>
    ```
    
    Literal (unnamed) struct:
    ```mlir
    !vdpp.struct<(i32, i64, i8)>
    ```
    
    Packed struct (no padding):
    ```mlir
    !vdpp.struct<packed (i32, i8, i32)>
    ```
  }];
  
  let parameters = (ins 
    OptionalParameter<"mlir::StringAttr">:$name,
    ArrayRefParameter<"mlir::Type">:$body,
    "bool":$isPacked
  );
  
  let builders = [
    // Named struct with explicit context
    TypeBuilder<(ins 
      "mlir::StringRef":$name,
      "llvm::ArrayRef<mlir::Type>":$body,
      CArg<"bool", "false">:$isPacked
    ), [{
      auto nameAttr = mlir::StringAttr::get($_ctxt, name);
      return $_get($_ctxt, nameAttr, body, isPacked);
    }]>,
    
    // Literal (unnamed) struct
    TypeBuilder<(ins 
      "llvm::ArrayRef<mlir::Type>":$body,
      CArg<"bool", "false">:$isPacked
    ), [{
      return $_get($_ctxt, nullptr, body, isPacked);
    }]>
  ];
  
  let hasCustomAssemblyFormat = 1;
  
  let extraClassDeclaration = [{
    /// Check if this is a named (identified) struct
    bool isIdentified() const { return getName() != nullptr; }
    
    /// Check if this is a literal (unnamed) struct
    bool isLiteral() const { return !isIdentified(); }
    
    /// Get the struct name as StringRef (empty for literal structs)
    llvm::StringRef getNameStr() const {
      if (auto n = getName())
        return n.getValue();
      return "";
    }
    
    /// Get the number of fields
    unsigned getNumElements() const { return getBody().size(); }
    
    /// Check if the struct is empty
    bool isEmpty() const { return getNumElements() == 0; }
    
    /// Get a specific element type
    mlir::Type getElementType(unsigned index) const {
      assert(index < getNumElements() && "index out of bounds");
      return getBody()[index];
    }
    
    /// Check if two structs have compatible layout
    bool isLayoutIdentical(StructType other) const {
      return getBody() == other.getBody() && 
             getIsPacked() == other.getIsPacked();
    }
  }];
}

#endif // VDPPTYPES