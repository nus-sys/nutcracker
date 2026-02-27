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

#endif // VDPPTYPES