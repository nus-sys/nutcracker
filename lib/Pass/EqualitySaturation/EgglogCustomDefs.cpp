#include "mlir/IR/Attributes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/AsmParser/AsmParser.h"
#include "Dialect/P4HIR/P4HIR_Attrs.h"
#include "Dialect/P4HIR/P4HIR_Types.h"
#include "Dialect/Bf3/Drmt/IR/Bf3DrmtAttrs.h"
#include "Dialect/Bf3/Drmt/IR/Bf3DrmtTypes.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"

#include "Pass/Egglog.h"
#include "Pass/EgglogCustomDefs.h"

/** 
 * Parse the attribute (function arith_fastmath (FastMathFlags) Attr)
 * Where (datatype FastMathFlags (none) (reassoc) (nnan) ...)
 */
mlir::Attribute parseFastMathFlagsAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "arith_fastmath");

    std::string flag = split[1];
    if (flag.front() == '(' && flag.back() == ')') {
        flag = flag.substr(1, flag.size() - 2);
    }

    std::string strAttr = "#arith.fastmath<" + flag + ">";
    mlir::Attribute parsedAttr = mlir::parseAttribute(strAttr, &egglog.context);

    // dump
    // llvm::outs() << "Parsing mlir::arith::FastMathFlagsAttr: " << strAttr << "\n";
    // llvm::outs() << "Parsed mlir::arith::FastMathFlagsAttr: " << parsedAttr << "\n";

    return parsedAttr;
}

/**
 * Serialize the attribute (function arith_fastmath (FastMathFlags) Attr)
 * Where (datatype FastMathFlags (none) (reassoc) (nnan) ...)
 */
std::vector<std::string> stringifyFastMathFlagsAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;

    split.push_back("arith_fastmath");

    mlir::arith::FastMathFlagsAttr fastMathAttr = attr.cast<mlir::arith::FastMathFlagsAttr>();
    mlir::arith::FastMathFlags flags = fastMathAttr.getValue();

    split.push_back("(" + mlir::arith::stringifyFastMathFlags(flags) + ")");

    // dump
    llvm::outs() << "Stringified mlir::arith::FastMathFlagsAttr: ";
    for (const std::string& s: split) {
        llvm::outs() << s << " , ";
    }
    llvm::outs() << "\n";

    return split;
}

/** Parse the type (function RankedTensor (IntVec Type) Type) */
mlir::Type parseRankedTensorType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "RankedTensor");

    std::string dimVec = split[1];
    std::string type = split[2];

    // parse dimvec, form (vec-of <N1> <N2> ... <Nn>)
    std::vector<std::string> dimVecSplit = Egglog::splitExpression(dimVec);
    std::vector<int64_t> dims;
    for (unsigned int i = 1; i < dimVecSplit.size(); i++) {
        dims.push_back(std::stoll(dimVecSplit[i]));
    }

    mlir::Type parsedType = egglog.parseType(type);  // parse type
    return mlir::RankedTensorType::get(dims, parsedType);
}

/** Serialize the type (function RankedTensor (IntVec Type) Type) */
std::vector<std::string> stringifyRankedTensorType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    split.push_back("RankedTensor");

    mlir::RankedTensorType tensorType = type.cast<mlir::RankedTensorType>();
    llvm::ArrayRef<int64_t> shape = tensorType.getShape();
    mlir::Type elementType = tensorType.getElementType();

    // serialize dimvec
    std::string dimVec = "(vec-of";
    for (int64_t dim: shape) {
        dimVec += " " + std::to_string(dim);
    }
    dimVec += ")";

    split.push_back(dimVec);
    split.push_back(egglog.eggifyType(elementType));

    return split;
}

/** Parse P4HIR extern type */
mlir::Type parseP4HIRExternType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_extern");

    if (split.size() < 2) {
        llvm::errs() << "Expected extern name for p4hir_extern\n";
        return {};
    }

    // Parse the extern name (should be quoted string)
    std::string externName = split[1];
    if (externName.front() == '"' && externName.back() == '"') {
        externName = externName.substr(1, externName.size() - 2);
    }

    // Parse optional type arguments
    llvm::SmallVector<mlir::Type> typeArgs;
    for (size_t i = 2; i < split.size(); ++i) {
        mlir::Type argType = egglog.parseType(split[i]);
        if (argType) {
            typeArgs.push_back(argType);
        }
    }

    // Create the extern type
    return P4::P4MLIR::P4HIR::ExternType::get(
        &egglog.context, externName, typeArgs);
}

/** Stringify P4HIR extern type */
std::vector<std::string> stringifyP4HIRExternType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    auto externType = mlir::dyn_cast<P4::P4MLIR::P4HIR::ExternType>(type);
    assert(externType && "Expected P4HIR ExternType");

    split.push_back("p4hir_extern");
    
    // Add the extern name with quotes
    split.push_back("\"" + externType.getName().str() + "\"");
    
    // Add type arguments if any
    for (auto argType : externType.getTypeArguments()) {
        split.push_back(egglog.eggifyType(argType));
    }

    return split;
}

/** 
 * Parse the attribute (function arith_fastmath (FastMathFlags) Attr)
 * Where (datatype FastMathFlags (none) (reassoc) (nnan) ...)
 */
mlir::Attribute parseP4HIRIntAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "p4hir_int");

    for (size_t i = 1; i < split.size(); i++) {
        llvm::outs() << "Parsing P4HIR IntAttr: " << split[i] << "\n";
    }

    // Parse the value (first argument)
    std::string valueStr = split[1];
    if (valueStr.front() == '(' && valueStr.back() == ')') {
        valueStr = valueStr.substr(1, valueStr.size() - 2);
    }
    int64_t value = std::stoll(valueStr);

    // Parse the type (second argument should be a bit type)
    std::string typeStr = split[2];
    mlir::Type type = egglog.parseType(typeStr);
    
    // Use mlir::dyn_cast instead of type.dyn_cast
    auto bitType = mlir::dyn_cast<P4::P4MLIR::P4HIR::BitsType>(type);
    assert(bitType && "P4HIR int attribute must have bit type");

    // Create APInt with the value and bit width
    llvm::APInt apValue(bitType.getWidth(), value, /*isSigned=*/true);

    // Create P4HIR integer attribute with correct parameter order: context, type, value
    return P4::P4MLIR::P4HIR::IntAttr::get(&egglog.context, type, apValue);
}

std::vector<std::string> stringifyP4HIRIntAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    
    // Cast to P4HIR IntAttr
    auto intAttr = mlir::dyn_cast<P4::P4MLIR::P4HIR::IntAttr>(attr);
    assert(intAttr && "Expected P4HIR IntAttr");

    // Get the value as APInt
    llvm::APInt value = intAttr.getValue();
    
    // Get the type
    mlir::Type type = intAttr.getType();

    split.push_back("p4hir_int");
    // Convert APInt to string - use getSExtValue() for signed interpretation
    split.push_back(std::to_string(value.getSExtValue()));
    // Add the type
    split.push_back(egglog.eggifyType(type));

    return split;
}

mlir::Attribute parseP4HIRBoolAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "p4hir_bool");

    for (size_t i = 1; i < split.size(); i++) {
        llvm::outs() << "Parsing P4HIR BoolAttr: " << split[i] << "\n";
    }

    // Parse the value (first argument)
    std::string valueStr = split[1];
    if (valueStr.front() == '(' && valueStr.back() == ')') {
        valueStr = valueStr.substr(1, valueStr.size() - 2);
    }
    
    bool value;
    if (valueStr == "true" || valueStr == "1") {
        value = true;
    } else if (valueStr == "false" || valueStr == "0") {  // Fix: compare string, not char
        value = false;
    } else {
        assert(false && "Invalid boolean value");
    }

    // Parse the type (second argument should be a bool type)
    std::string typeStr = split[2];
    mlir::Type type = egglog.parseType(typeStr);
    
    auto boolType = mlir::dyn_cast<P4::P4MLIR::P4HIR::BoolType>(type);
    assert(boolType && "P4HIR bool attribute must have bool type");

    // Create P4HIR boolean attribute with correct parameter order: context, type, value
    return P4::P4MLIR::P4HIR::BoolAttr::get(&egglog.context, type, value);
}

std::vector<std::string> stringifyP4HIRBoolAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    
    // Cast to P4HIR BoolAttr
    auto boolAttr = mlir::dyn_cast<P4::P4MLIR::P4HIR::BoolAttr>(attr);
    assert(boolAttr && "Expected P4HIR BoolAttr");

    // Get the value
    bool value = boolAttr.getValue();
    
    // Get the type
    mlir::Type type = boolAttr.getType();

    split.push_back("p4hir_bool");
    // Convert bool to string
    split.push_back(value ? "true" : "false");
    // Add the type
    split.push_back(egglog.eggifyType(type));

    return split;
}

/** Parse P4HIR ValidityBit attribute */
mlir::Attribute parseP4HIRValidityBitAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "p4hir_validity_bit");

    for (size_t i = 1; i < split.size(); i++) {
        llvm::outs() << "Parsing P4HIR ValidityBitAttr: " << split[i] << "\n";
    }

    // Parse the validity value (should be "valid" or "invalid")
    std::string validityStr = split[1];
    
    // Remove quotes if present
    if (validityStr.front() == '"' && validityStr.back() == '"') {
        validityStr = validityStr.substr(1, validityStr.size() - 2);
    }
    // Remove parentheses if present
    if (validityStr.front() == '(' && validityStr.back() == ')') {
        validityStr = validityStr.substr(1, validityStr.size() - 2);
    }

    P4::P4MLIR::P4HIR::ValidityBit validityValue;
    if (validityStr == "valid") {
        validityValue = P4::P4MLIR::P4HIR::ValidityBit::Valid;
    } else if (validityStr == "invalid") {
        validityValue = P4::P4MLIR::P4HIR::ValidityBit::Invalid;
    } else {
        assert(false && "Invalid validity bit value");
    }

    // Parse the type (should be ValidBitType)
    std::string typeStr = split[2];
    mlir::Type type = egglog.parseType(typeStr);

    // Create the ValidityBitAttr
    return P4::P4MLIR::P4HIR::ValidityBitAttr::get(&egglog.context, validityValue);
}

std::vector<std::string> stringifyP4HIRValidityBitAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    
    auto validityAttr = mlir::dyn_cast<P4::P4MLIR::P4HIR::ValidityBitAttr>(attr);
    assert(validityAttr && "Expected P4HIR ValidityBit");

    // Get the validity value
    P4::P4MLIR::P4HIR::ValidityBit value = validityAttr.getValue();
    
    split.push_back("p4hir_validity_bit");
    
    // Convert enum to string with proper quoting
    if (value == P4::P4MLIR::P4HIR::ValidityBit::Valid) {
        split.push_back("\"valid\"");
    } else if (value == P4::P4MLIR::P4HIR::ValidityBit::Invalid) {
        split.push_back("\"invalid\"");
    } else {
        assert(false && "Unknown validity bit value");
    }
    
    // Add the type
    split.push_back(egglog.eggifyType(validityAttr.getType()));

    return split;
}

mlir::Attribute parseP4HIRMatchKindAttr(const std::vector<std::string>& split, Egglog &egglog) {
    std::string attrType = split[0];
    assert(attrType == "p4hir_match_kind");

    if (split.size() < 2) {
        llvm::errs() << "Expected value for p4hir_match_kind\n";
        return {};
    }

    // Second token is the match kind string (e.g., "exact")
    std::string valueStr = split[1];

    // Remove surrounding parentheses/quotes if needed
    if (valueStr.front() == '"' && valueStr.back() == '"')
        valueStr = valueStr.substr(1, valueStr.size() - 2);

    // Build the MLIR attribute
    return P4::P4MLIR::P4HIR::MatchKindAttr::get(
        &egglog.context, mlir::StringAttr::get(&egglog.context, valueStr));
}

std::vector<std::string> stringifyP4HIRMatchKindAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    auto mkAttr = mlir::dyn_cast<P4::P4MLIR::P4HIR::MatchKindAttr>(attr);
    assert(mkAttr && "Expected MatchKindAttr");

    split.push_back("p4hir_match_kind");
    // Preserve quotes so Egglog knows it's a string
    split.push_back("\"" + mkAttr.getValue().getValue().str() + "\"");
    return split;
}

/** Parse P4HIR bit type (function p4hir.bits (Int) Type) */
mlir::Type parseP4HIRBitsType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_bits");

    // Parse width parameter
    std::string width = split[1];
    if (width.front() == '(' && width.back() == ')') {
        width = width.substr(1, width.size() - 2);
    }

    // Create the type string and parse it
    std::string strType = "!p4hir.bit<" + width + ">";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Stringify P4HIR bit type */
std::vector<std::string> stringifyP4HIRBitsType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    // Get the bit width from the type
    auto bitsType = type.cast<P4::P4MLIR::P4HIR::BitsType>();
    unsigned width = bitsType.getWidth();

    split.push_back("p4hir_bits");
    split.push_back(std::to_string(width));

    return split;
}

/** Parse P4HIR valid bit type (function p4hir_valid_bit Type) */
mlir::Type parseP4HIRValidBitType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_valid_bit");

    // Valid bit has no parameters
    std::string strType = "!p4hir.valid.bit";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Stringify P4HIR valid bit type */
std::vector<std::string> stringifyP4HIRValidBitType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;

    llvm::outs() << "Stringifying P4HIR ValidBitType\n";

    // Just return the tag
    (void)egglog; // not needed
    auto validBitType = type.cast<P4::P4MLIR::P4HIR::ValidBitType>();
    split.push_back("p4hir_valid_bit");

    return split;
}

/** Parse P4HIR bool type (function p4hir_bool () Type) */
mlir::Type parseP4HIRBoolType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_bool");

    // BoolType takes no parameters, so we expect only the type name
    assert(split.size() == 1 && "P4HIR bool type takes no parameters");

    // Create the type string and parse it
    std::string strType = "!p4hir.bool";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Stringify P4HIR bool type */
std::vector<std::string> stringifyP4HIRBoolType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;

    llvm::outs() << "Stringifying P4HIR BoolType\n";
    
    // Verify it's actually a BoolType and use mlir::dyn_cast instead of type.cast
    auto boolType = mlir::dyn_cast<P4::P4MLIR::P4HIR::BoolType>(type);
    assert(boolType && "Expected P4HIR BoolType");

    split.push_back("p4hir_bool");
    // No additional parameters needed for bool type

    return split;
}

/** Parse P4HIR infint type (function p4hir_infint () Type) */
mlir::Type parseP4HIRInfIntType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_infint");

    // InfIntType takes no parameters, so we expect only the type name
    assert(split.size() == 1 && "P4HIR infint type takes no parameters");

    // Create the type string and parse it
    std::string strType = "!p4hir.infint";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Stringify P4HIR infint type */
std::vector<std::string> stringifyP4HIRInfIntType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;

    llvm::outs() << "Stringifying P4HIR InfIntType\n";
    
    // Verify it's actually an InfIntType
    auto infintType = mlir::dyn_cast<P4::P4MLIR::P4HIR::InfIntType>(type);
    assert(infintType && "Expected P4HIR InfIntType");

    split.push_back("p4hir_infint");
    // No additional parameters needed for infint type

    return split;
}

/** Parse P4HIR error type (function p4hir_error Type) */
mlir::Type parseP4HIRErrorType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_error");

    // Error type format: p4hir_error field1 field2 field3 ...
    // Build the type string: !p4hir.error<field1, field2, field3>
    std::string strType = "!p4hir.error";
    
    if (split.size() > 1) {
        strType += "<";
        for (size_t i = 1; i < split.size(); ++i) {
            if (i > 1) {
                strType += ", ";
            }
            strType += split[i];
        }
        strType += ">";
    } else {
        // Empty error type
        strType += "<>";
    }

    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);
    return parsedType;
}

/** Stringify P4HIR error type */
std::vector<std::string> stringifyP4HIRErrorType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    (void)egglog; // not needed
    auto errorType = type.cast<P4::P4MLIR::P4HIR::ErrorType>();
    
    // Start with the type tag
    split.push_back("p4hir_error");
    
    // Add each field from the error type
    mlir::ArrayAttr fields = errorType.getFields();
    for (mlir::Attribute field : fields) {
        if (auto stringAttr = field.dyn_cast<mlir::StringAttr>()) {
            split.push_back(stringAttr.getValue().str());
        }
    }
    
    return split;
}

mlir::Type parseP4HIRHeaderType(const std::vector<std::string> &split, Egglog &egglog) {
    assert(split[0] == "p4hir_header");
    llvm::SmallVector<P4::P4MLIR::P4HIR::FieldInfo> fields;
    std::string headerName;
    bool hasValidityBit = false;

    for (size_t i = 1; i < split.size(); i++) {
        std::string attrStr = split[i];
        // Find NamedAttr
        size_t namedAttrPos = attrStr.find("NamedAttr");
        if (namedAttrPos == std::string::npos) {
            continue;
        }

        // Extract key
        size_t nameStart = attrStr.find('"', namedAttrPos);
        size_t nameEnd   = attrStr.find('"', nameStart + 1);
        if (nameStart == std::string::npos || nameEnd == std::string::npos) {
            llvm::outs() << "    -> skipped (no quoted key)\n";
            continue;
        }
        std::string key = attrStr.substr(nameStart + 1, nameEnd - nameStart - 1);

        if (key == "__valid") {
            hasValidityBit = true;
            continue;
        }

        if (key == "name") {
            // parse header name
            size_t stringAttrPos = attrStr.find("StringAttr", nameEnd);
            if (stringAttrPos != std::string::npos) {
                size_t valueStart = attrStr.find('"', stringAttrPos);
                size_t valueEnd   = attrStr.find('"', valueStart + 1);
                if (valueStart != std::string::npos && valueEnd != std::string::npos) {
                    headerName = attrStr.substr(valueStart + 1, valueEnd - valueStart - 1);
                }
            }
        } else {
            // Parse TypeAttr field
            mlir::Type fieldType = nullptr;
            size_t typeAttrPos = attrStr.find("TypeAttr", nameEnd);
            if (typeAttrPos != std::string::npos) {
                size_t typeStart = attrStr.find('(', typeAttrPos);
                if (typeStart != std::string::npos) {
                    // match parentheses
                    int depth = 0;
                    size_t typeEnd = typeStart;
                    for (size_t j = typeStart; j < attrStr.length(); j++) {
                        if (attrStr[j] == '(') depth++;
                        else if (attrStr[j] == ')') depth--;
                        if (depth == 0) {
                            typeEnd = j;
                            break;
                        }
                    }

                    std::string typeStr = attrStr.substr(typeStart + 1, typeEnd - typeStart - 1);

                    // Special case: p4hir_bits
                    if (typeStr.find("p4hir_bits") == 0) {
                        size_t spacePos = typeStr.find(' ');
                        if (spacePos != std::string::npos) {
                            std::string widthStr = typeStr.substr(spacePos + 1);
                            unsigned width = static_cast<unsigned>(std::stoi(widthStr));
                            std::string strType = "!p4hir.bit<" + std::to_string(width) + ">";
                            fieldType = mlir::parseType(strType, &egglog.context);
                        }
                    } else {
                        fieldType = mlir::parseType(typeStr, &egglog.context);
                    }
                }
            }

            if (fieldType) {
                fields.push_back(
                    P4::P4MLIR::P4HIR::FieldInfo{
                        mlir::StringAttr::get(&egglog.context, key), 
                        fieldType
                    });
            } else {
                llvm::outs() << "    -> FAILED to parse type for field '" << key << "'\n";
            }
        }
    }
#if 0
    // Inject validity bit if not present
    if (!hasValidityBit) {
        auto validType = P4::P4MLIR::P4HIR::ValidBitType::get(&egglog.context);
        llvm::outs() << "  -> Injecting implicit __valid field\n";
        fields.push_back(
            P4::P4MLIR::P4HIR::FieldInfo{
                mlir::StringAttr::get(&egglog.context, "__valid"), 
                validType
            });
    }
#endif
    // llvm::outs() << "[parseP4HIRHeaderType] Final header = " << headerName 
    //              << ", field count = " << fields.size() << "\n";
    // for (auto &f : fields) {
    //     llvm::outs() << "    field '" << f.name.getValue() << "' : ";
    //     f.type.print(llvm::outs());
    //     llvm::outs() << "\n";
    // }

    mlir::DictionaryAttr annots = mlir::DictionaryAttr::get(&egglog.context, {});
    return P4::P4MLIR::P4HIR::HeaderType::get(&egglog.context, headerName, fields, annots);
}

std::vector<std::string> stringifyP4HIRHeaderType(mlir::Type type, Egglog &egglog) {
    std::vector<std::string> split;
    auto headerType = type.cast<P4::P4MLIR::P4HIR::HeaderType>();

    // name attribute
    split.push_back("p4hir_header (NamedAttr \"name\" (StringAttr \"" +
                    headerType.getName().str() + "\"))");

    // fields (excluding __valid)
    for (auto field : headerType.getFields()) {
        if (field.name == P4::P4MLIR::P4HIR::HeaderType::validityBit)
            continue;

        std::string fieldStr = "(NamedAttr \"" + field.name.str() + "\" (TypeAttr ";
        if (auto bitsType = field.type.dyn_cast<P4::P4MLIR::P4HIR::BitsType>()) {
            fieldStr += "(p4hir_bits " + std::to_string(bitsType.getWidth()) + ")";
        } else {
            std::string typeStr;
            llvm::raw_string_ostream rso(typeStr);
            field.type.print(rso);
            rso.flush();
            fieldStr += typeStr;
        }
        fieldStr += "))";
        split.push_back(fieldStr);
    }

    return split;
}

mlir::Type parseP4HIRStructType(const std::vector<std::string> &split,
                                Egglog &egglog) {
  assert(split[0] == "p4hir_struct");

  // First NamedAttr is the struct name
  std::string structName;
  llvm::SmallVector<P4::P4MLIR::P4HIR::FieldInfo> fields;

  for (size_t i = 1; i < split.size(); i++) {
    std::string fieldStr = split[i];

    size_t namedAttrPos = fieldStr.find("NamedAttr");
    if (namedAttrPos == std::string::npos)
      continue;

    // ---- Extract key ----
    size_t nameStart = fieldStr.find('"', namedAttrPos);
    size_t nameEnd = fieldStr.find('"', nameStart + 1);
    if (nameStart == std::string::npos || nameEnd == std::string::npos)
      continue;

    std::string key = fieldStr.substr(nameStart + 1, nameEnd - nameStart - 1);

    // ---- Special case: struct name ----
    if (key == "name") {
      size_t strStart = fieldStr.find('"', nameEnd + 1);
      size_t strEnd = fieldStr.find('"', strStart + 1);
      if (strStart != std::string::npos && strEnd != std::string::npos) {
        structName = fieldStr.substr(strStart + 1, strEnd - strStart - 1);
      }
      continue;
    }

    // ---- Otherwise it's a field ----
    // Extract type string inside (TypeAttr ...)
    size_t typeAttrPos = fieldStr.find("TypeAttr", nameEnd);
    if (typeAttrPos == std::string::npos)
      continue;

    size_t typeStart = fieldStr.find('(', typeAttrPos);
    if (typeStart == std::string::npos)
      continue;

    int depth = 0;
    size_t typeEnd = typeStart;
    for (size_t j = typeStart; j < fieldStr.size(); j++) {
      if (fieldStr[j] == '(')
        depth++;
      else if (fieldStr[j] == ')')
        depth--;
      if (depth == 0) {
        typeEnd = j;
        break;
      }
    }

    std::string typeStr =
        fieldStr.substr(typeStart + 1, typeEnd - typeStart - 1);

    // ---- Parse field type ----
    mlir::Type fieldType;

    if (typeStr.find("p4hir_header") == 0) {
      // Collect header tokens
      std::vector<std::string> headerTokens;
      headerTokens.push_back("p4hir_header");

      size_t pos = 0;
      while ((pos = typeStr.find("(NamedAttr", pos)) != std::string::npos) {
        int d = 0;
        size_t start = pos, end = pos;
        for (size_t k = pos; k < typeStr.size(); k++) {
          if (typeStr[k] == '(')
            d++;
          else if (typeStr[k] == ')')
            d--;
          if (d == 0) {
            end = k + 1;
            break;
          }
        }
        headerTokens.push_back(typeStr.substr(start, end - start));
        pos = end;
      }

      fieldType = parseP4HIRHeaderType(headerTokens, egglog);

    } else if (typeStr.find("p4hir_bits") == 0) {
      // p4hir_bits N
      size_t spacePos = typeStr.find(' ');
      if (spacePos != std::string::npos) {
        std::string widthStr = typeStr.substr(spacePos + 1);
        size_t parenPos = widthStr.find(')');
        if (parenPos != std::string::npos)
          widthStr = widthStr.substr(0, parenPos);

        unsigned width = static_cast<unsigned>(std::stoi(widthStr));
        std::string strType = "!p4hir.bit<" + std::to_string(width) + ">";
        fieldType = mlir::parseType(strType, &egglog.context);
      }
    } else {
      // Fallback: regular MLIR type
      fieldType = mlir::parseType(typeStr, &egglog.context);
    }

    if (fieldType) {
      fields.push_back(
          {mlir::StringAttr::get(&egglog.context, key), fieldType});
    }
  }

  mlir::DictionaryAttr annots =
      mlir::DictionaryAttr::get(&egglog.context, {});
  return P4::P4MLIR::P4HIR::StructType::get(&egglog.context, structName,
                                            fields, annots);
}

std::vector<std::string> stringifyP4HIRStructType(mlir::Type type,
                                                  Egglog &egglog) {
  std::vector<std::string> split;
  auto structType = type.cast<P4::P4MLIR::P4HIR::StructType>();

  split.push_back("p4hir_struct");

  // First, emit the struct name
  split.push_back("(NamedAttr \"name\" (StringAttr \"" +
                  structType.getName().str() + "\"))");

  // Then, emit fields
  for (auto field : structType.getFields()) {
    std::string fieldStr = "(NamedAttr \"" + field.name.str() +
                           "\" (TypeAttr ";

    if (auto hdrType =
            field.type.dyn_cast<P4::P4MLIR::P4HIR::HeaderType>()) {
      // Delegate header
      auto sub = stringifyP4HIRHeaderType(hdrType, egglog);
      fieldStr += "(" + llvm::join(sub, " ") + ")";
    } else if (auto bitsType =
                   field.type.dyn_cast<P4::P4MLIR::P4HIR::BitsType>()) {
      fieldStr += "(p4hir_bits " +
                  std::to_string(bitsType.getWidth()) + ")";
    } else {
      std::string typeStr;
      llvm::raw_string_ostream rso(typeStr);
      field.type.print(rso);
      rso.flush();
      fieldStr += typeStr;
    }

    fieldStr += "))";
    split.push_back(fieldStr);
  }

  return split;
}

/** Parse P4HIR reference type (function p4hir_ref (Type) Type) */
mlir::Type parseP4HIRReferenceType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "p4hir_ref");

    // Parse inner type parameter
    std::string innerTypeStr = split[1];
    
    // Use egglog's parseType method like in parseRankedTensorType
    mlir::Type innerType = egglog.parseType(innerTypeStr);
    if (!innerType) {
        llvm::errs() << "Failed to parse inner type: " << innerTypeStr << "\n";
        return nullptr;
    }

    // Create reference type using P4HIR's RefType::get
    return P4::P4MLIR::P4HIR::ReferenceType::get(innerType);
}

/** Stringify P4HIR reference type */
std::vector<std::string> stringifyP4HIRReferenceType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;

    // llvm::outs() << "Stringifying P4HIR ReferenceType\n";
    
    // Get the inner type from the reference type
    auto refType = type.cast<P4::P4MLIR::P4HIR::ReferenceType>();
    mlir::Type innerType = refType.getObjectType();

    // Get the string representation of the inner type using egglog's eggifyType
    std::string innerTypeStr = egglog.eggifyType(innerType);
    
    split.push_back("p4hir_ref");
    // Add the inner type WITHOUT extra parentheses
    split.push_back(innerTypeStr);  // Remove the extra parentheses

    return split;
}

/* ------- bf3drmt ------- */
mlir::Attribute parseBf3DrmtIntAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "bf3drmt_int");

    // Parse the value (first argument)
    std::string valueStr = split[1];
    if (valueStr.front() == '(' && valueStr.back() == ')') {
        valueStr = valueStr.substr(1, valueStr.size() - 2);
    }
    int64_t value = std::stoll(valueStr);

    // Parse the type (second argument should be a bit type)
    std::string typeStr = split[2];
    mlir::Type type = egglog.parseType(typeStr);
    
    // Use mlir::dyn_cast instead of type.dyn_cast
    auto bitType = mlir::dyn_cast<mlir::edamlir::bf3drmt::BitsType>(type);
    assert(bitType && "Bf3 Drmt int attribute must have bit type");

    // Create APInt with the value and bit width
    llvm::APInt apValue(bitType.getWidth(), value, /*isSigned=*/true);

    // Create P4HIR integer attribute with correct parameter order: context, type, value
    return mlir::edamlir::bf3drmt::IntAttr::get(&egglog.context, type, apValue);
}

std::vector<std::string> stringifyBf3DrmtIntAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    
    auto intAttr = mlir::dyn_cast<mlir::edamlir::bf3drmt::IntAttr>(attr);
    assert(intAttr && "Expected Bf3 Drmt IntAttr");

    // Get the value as APInt
    llvm::APInt value = intAttr.getValue();
    
    // Get the type
    mlir::Type type = intAttr.getType();

    split.push_back("bf3drmt_int");
    // Convert APInt to string - use getSExtValue() for signed interpretation
    split.push_back(std::to_string(value.getSExtValue()));
    // Add the type
    split.push_back(egglog.eggifyType(type));

    return split;
}

mlir::Attribute parseBf3DrmtBoolAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "p4hir_bool");

    for (size_t i = 1; i < split.size(); i++) {
        llvm::outs() << "Parsing P4HIR BoolAttr: " << split[i] << "\n";
    }

    // Parse the value (first argument)
    std::string valueStr = split[1];
    if (valueStr.front() == '(' && valueStr.back() == ')') {
        valueStr = valueStr.substr(1, valueStr.size() - 2);
    }
    
    bool value;
    if (valueStr == "true" || valueStr == "1") {
        value = true;
    } else if (valueStr == "false" || valueStr == "0") {  // Fix: compare string, not char
        value = false;
    } else {
        assert(false && "Invalid boolean value");
    }

    // Parse the type (second argument should be a bool type)
    std::string typeStr = split[2];
    mlir::Type type = egglog.parseType(typeStr);
    
    auto boolType = mlir::dyn_cast<mlir::edamlir::bf3drmt::BoolType>(type);
    assert(boolType && "P4HIR bool attribute must have bool type");

    // Create P4HIR boolean attribute with correct parameter order: context, type, value
    return mlir::edamlir::bf3drmt::BoolAttr::get(&egglog.context, type, value);
}

std::vector<std::string> stringifyBf3DrmtBoolAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    
    // Cast to P4HIR BoolAttr
    auto boolAttr = mlir::dyn_cast<mlir::edamlir::bf3drmt::BoolAttr>(attr);
    assert(boolAttr && "Expected P4HIR BoolAttr");

    // Get the value
    bool value = boolAttr.getValue();
    
    // Get the type
    mlir::Type type = boolAttr.getType();

    split.push_back("p4hir_bool");
    // Convert bool to string
    split.push_back(value ? "true" : "false");
    // Add the type
    split.push_back(egglog.eggifyType(type));

    return split;
}

mlir::Attribute parseBf3DrmtValidityBitAttr(const std::vector<std::string>& split, Egglog& egglog) {
    std::string attrType = split[0];
    assert(attrType == "bf3drmt_validity_bit");

    for (size_t i = 1; i < split.size(); i++) {
        llvm::outs() << "Parsing P4HIR ValidityBitAttr: " << split[i] << "\n";
    }

    // Parse the validity value (should be "valid" or "invalid")
    std::string validityStr = split[1];
    
    // Remove quotes if present
    if (validityStr.front() == '"' && validityStr.back() == '"') {
        validityStr = validityStr.substr(1, validityStr.size() - 2);
    }
    // Remove parentheses if present
    if (validityStr.front() == '(' && validityStr.back() == ')') {
        validityStr = validityStr.substr(1, validityStr.size() - 2);
    }

    mlir::edamlir::bf3drmt::ValidityBit validityValue;
    if (validityStr == "valid") {
        validityValue = mlir::edamlir::bf3drmt::ValidityBit::Valid;
    } else if (validityStr == "invalid") {
        validityValue = mlir::edamlir::bf3drmt::ValidityBit::Invalid;
    } else {
        assert(false && "Invalid validity bit value");
    }

    // Parse the type (should be ValidBitType)
    std::string typeStr = split[2];
    mlir::Type type = egglog.parseType(typeStr);

    // Create the ValidityBitAttr
    return mlir::edamlir::bf3drmt::ValidityBitAttr::get(&egglog.context, validityValue);
}

/** Stringify P4HIR ValidityBit attribute */
std::vector<std::string> stringifyBf3DrmtValidityBitAttr(mlir::Attribute attr, Egglog& egglog) {
    std::vector<std::string> split;
    
    auto validityAttr = mlir::dyn_cast<mlir::edamlir::bf3drmt::ValidityBitAttr>(attr);
    assert(validityAttr && "Expected P4HIR ValidityBit");

    // Get the validity value
    mlir::edamlir::bf3drmt::ValidityBit value = validityAttr.getValue();
    
    split.push_back("bf3drmt_validity_bit");
    
    // Convert enum to string with proper quoting
    if (value == mlir::edamlir::bf3drmt::ValidityBit::Valid) {
        split.push_back("\"valid\"");
    } else if (value == mlir::edamlir::bf3drmt::ValidityBit::Invalid) {
        split.push_back("\"invalid\"");
    } else {
        assert(false && "Unknown validity bit value");
    }
    
    // Add the type
    split.push_back(egglog.eggifyType(validityAttr.getType()));

    return split;
}

/** Parse P4HIR bool type (function p4hir_bool () Type) */
mlir::Type parseBf3DrmtBoolType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "bf3drmt_bool");

    // BoolType takes no parameters, so we expect only the type name
    assert(split.size() == 1 && "Bf3 Drmt bool type takes no parameters");

    // Create the type string and parse it
    std::string strType = "!bf3drmt.bool";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Stringify P4HIR bool type */
std::vector<std::string> stringifyBf3DrmtBoolType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;

    llvm::outs() << "Stringifying Bf3 Drmt BoolType\n";
    
    // Verify it's actually a BoolType and use mlir::dyn_cast instead of type.cast
    auto boolType = mlir::dyn_cast<P4::P4MLIR::P4HIR::BoolType>(type);
    assert(boolType && "Expected P4HIR BoolType");

    split.push_back("bf3drmt_bool");
    // No additional parameters needed for bool type

    return split;
}

/** Parse Bf3Drmt bit type (function bf3drmt.bits (Int) Type) */
mlir::Type parseBf3DrmtBitsType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "bf3drmt_bits");

    // Parse width parameter
    std::string width = split[1];
    if (width.front() == '(' && width.back() == ')') {
        width = width.substr(1, width.size() - 2);
    }

    // Create the type string and parse it
    std::string strType = "!bf3drmt.bit<" + width + ">";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Stringify Bf3Drmt bit type */
std::vector<std::string> stringifyBf3DrmtBitsType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    // Get the bit width from the type
    auto bitsType = type.cast<mlir::edamlir::bf3drmt::BitsType>();
    unsigned width = bitsType.getWidth();

    split.push_back("bf3drmt_bits");
    split.push_back(std::to_string(width));

    return split;
}

/** Parse Bf3Drmt valid bit type (function bf3drmt_valid_bit Type) */
mlir::Type parseBf3DrmtValidBitType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "bf3drmt_valid_bit");

    // Valid bit has no parameters
    std::string strType = "!bf3drmt.validity.bit";
    mlir::Type parsedType = mlir::parseType(strType, &egglog.context);

    return parsedType;
}

/** Parse bf3drmt MatchKind attribute */
mlir::Attribute parseBf3DrmtMatchKindAttr(const std::vector<std::string>& split, Egglog &egglog) {
    std::string attrType = split[0];
    assert(attrType == "bf3drmt_match_kind");

    if (split.size() < 2) {
        llvm::errs() << "Expected value for bf3drmt_match_kind\n";
        return {};
    }

    // Second token is the match kind string (e.g., "exact")
    std::string valueStr = split[1];

    // Remove surrounding quotes if needed
    if (valueStr.front() == '"' && valueStr.back() == '"')
        valueStr = valueStr.substr(1, valueStr.size() - 2);

    // Build the MLIR attribute
    return mlir::edamlir::bf3drmt::MatchKindAttr::get(
        &egglog.context, mlir::StringAttr::get(&egglog.context, valueStr));
}

/** Stringify bf3drmt MatchKind attribute */
std::vector<std::string> stringifyBf3DrmtMatchKindAttr(mlir::Attribute attr, Egglog &egglog) {
    std::vector<std::string> split;
    
    auto matchKindAttr = mlir::dyn_cast<mlir::edamlir::bf3drmt::MatchKindAttr>(attr);
    assert(matchKindAttr && "Expected bf3drmt MatchKindAttr");

    split.push_back("bf3drmt_match_kind");
    
    // Get the string value and add quotes
    std::string value = matchKindAttr.getValue().str();
    split.push_back("\"" + value + "\"");

    return split;
}

/** Stringify Bf3Drmt valid bit type */
std::vector<std::string> stringifyBf3DrmtValidBitType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;

    // Just return the tag
    (void)egglog; // not needed
    auto validBitType = type.cast<mlir::edamlir::bf3drmt::ValidBitType>();
    split.push_back("bf3drmt_valid_bit");

    return split;
}

mlir::Type parseBf3DrmtHeaderType(const std::vector<std::string> &split, Egglog &egglog) {
    assert(split[0] == "bf3drmt_header");
    llvm::SmallVector<mlir::edamlir::bf3drmt::FieldInfo> fields;
    std::string headerName;
    bool hasValidityBit = false;

    for (size_t i = 1; i < split.size(); i++) {
        std::string attrStr = split[i];
        // Find NamedAttr
        size_t namedAttrPos = attrStr.find("NamedAttr");
        if (namedAttrPos == std::string::npos) {
            continue;
        }

        // Extract key
        size_t nameStart = attrStr.find('"', namedAttrPos);
        size_t nameEnd   = attrStr.find('"', nameStart + 1);
        if (nameStart == std::string::npos || nameEnd == std::string::npos) {
            llvm::outs() << "    -> skipped (no quoted key)\n";
            continue;
        }
        std::string key = attrStr.substr(nameStart + 1, nameEnd - nameStart - 1);

        if (key == "__valid") {
            hasValidityBit = true;
            continue;
        }

        if (key == "name") {
            // parse header name
            size_t stringAttrPos = attrStr.find("StringAttr", nameEnd);
            if (stringAttrPos != std::string::npos) {
                size_t valueStart = attrStr.find('"', stringAttrPos);
                size_t valueEnd   = attrStr.find('"', valueStart + 1);
                if (valueStart != std::string::npos && valueEnd != std::string::npos) {
                    headerName = attrStr.substr(valueStart + 1, valueEnd - valueStart - 1);
                }
            }
        } else {
            // Parse TypeAttr field
            mlir::Type fieldType = nullptr;
            size_t typeAttrPos = attrStr.find("TypeAttr", nameEnd);
            if (typeAttrPos != std::string::npos) {
                size_t typeStart = attrStr.find('(', typeAttrPos);
                if (typeStart != std::string::npos) {
                    // match parentheses
                    int depth = 0;
                    size_t typeEnd = typeStart;
                    for (size_t j = typeStart; j < attrStr.length(); j++) {
                        if (attrStr[j] == '(') depth++;
                        else if (attrStr[j] == ')') depth--;
                        if (depth == 0) {
                            typeEnd = j;
                            break;
                        }
                    }

                    std::string typeStr = attrStr.substr(typeStart + 1, typeEnd - typeStart - 1);

                    // Special case: bf3drmt_bits
                    if (typeStr.find("bf3drmt_bits") == 0) {
                        size_t spacePos = typeStr.find(' ');
                        if (spacePos != std::string::npos) {
                            std::string widthStr = typeStr.substr(spacePos + 1);
                            unsigned width = static_cast<unsigned>(std::stoi(widthStr));
                            std::string strType = "!bf3drmt.bit<" + std::to_string(width) + ">";
                            fieldType = mlir::parseType(strType, &egglog.context);
                        }
                    } else {
                        fieldType = mlir::parseType(typeStr, &egglog.context);
                    }
                }
            }

            if (fieldType) {
                fields.push_back(mlir::edamlir::bf3drmt::FieldInfo{mlir::StringAttr::get(&egglog.context, key), fieldType});
            } else {
                llvm::outs() << "    -> FAILED to parse type for field '" << key << "'\n";
            }
        }
    }

    mlir::DictionaryAttr annots = mlir::DictionaryAttr::get(&egglog.context, {});
    return mlir::edamlir::bf3drmt::HeaderType::get(&egglog.context, headerName, fields, annots);
}

std::vector<std::string> stringifyBf3DrmtHeaderType(mlir::Type type, Egglog &egglog) {
    std::vector<std::string> split;
    auto headerType = type.cast<mlir::edamlir::bf3drmt::HeaderType>();

    // name attribute
    split.push_back("bf3drmt_header (NamedAttr \"name\" (StringAttr \"" +
                    headerType.getName().str() + "\"))");

    // fields (excluding __valid)
    for (auto field : headerType.getFields()) {
        if (field.name == mlir::edamlir::bf3drmt::HeaderType::validityBit)
            continue;

        std::string fieldStr = "(NamedAttr \"" + field.name.str() + "\" (TypeAttr ";
        if (auto bitsType = field.type.dyn_cast<mlir::edamlir::bf3drmt::BitsType>()) {
            fieldStr += "(bf3drmt_bits " + std::to_string(bitsType.getWidth()) + ")";
        } else {
            std::string typeStr;
            llvm::raw_string_ostream rso(typeStr);
            field.type.print(rso);
            rso.flush();
            fieldStr += typeStr;
        }
        fieldStr += "))";
        split.push_back(fieldStr);
    }

    return split;
}

mlir::Type parseBf3DrmtStructType(const std::vector<std::string> &split, Egglog &egglog) {
  assert(split[0] == "bf3drmt_struct");

  llvm::outs() << " >> Parsing Bf3Drmt struct type: \n";

  // First NamedAttr is the struct name
  std::string structName;
  llvm::SmallVector<mlir::edamlir::bf3drmt::FieldInfo> fields;

  for (size_t i = 1; i < split.size(); i++) {
    std::string fieldStr = split[i];

    size_t namedAttrPos = fieldStr.find("NamedAttr");
    if (namedAttrPos == std::string::npos)
      continue;

    // ---- Extract key ----
    size_t nameStart = fieldStr.find('"', namedAttrPos);
    size_t nameEnd = fieldStr.find('"', nameStart + 1);
    if (nameStart == std::string::npos || nameEnd == std::string::npos)
      continue;

    std::string key = fieldStr.substr(nameStart + 1, nameEnd - nameStart - 1);

    // ---- Special case: struct name ----
    if (key == "name") {
      size_t strStart = fieldStr.find('"', nameEnd + 1);
      size_t strEnd = fieldStr.find('"', strStart + 1);
      if (strStart != std::string::npos && strEnd != std::string::npos) {
        structName = fieldStr.substr(strStart + 1, strEnd - strStart - 1);
      }
      continue;
    }

    // ---- Otherwise it's a field ----
    // Extract type string inside (TypeAttr ...)
    size_t typeAttrPos = fieldStr.find("TypeAttr", nameEnd);
    if (typeAttrPos == std::string::npos)
      continue;

    size_t typeStart = fieldStr.find('(', typeAttrPos);
    if (typeStart == std::string::npos)
      continue;

    int depth = 0;
    size_t typeEnd = typeStart;
    for (size_t j = typeStart; j < fieldStr.size(); j++) {
      if (fieldStr[j] == '(')
        depth++;
      else if (fieldStr[j] == ')')
        depth--;
      if (depth == 0) {
        typeEnd = j;
        break;
      }
    }

    std::string typeStr =
        fieldStr.substr(typeStart + 1, typeEnd - typeStart - 1);

    // ---- Parse field type ----
    mlir::Type fieldType;

    if (typeStr.find("bf3drmt_header") == 0) {
      // Collect header tokens
      std::vector<std::string> headerTokens;
      headerTokens.push_back("bf3drmt_header");

      size_t pos = 0;
      while ((pos = typeStr.find("(NamedAttr", pos)) != std::string::npos) {
        int d = 0;
        size_t start = pos, end = pos;
        for (size_t k = pos; k < typeStr.size(); k++) {
          if (typeStr[k] == '(')
            d++;
          else if (typeStr[k] == ')')
            d--;
          if (d == 0) {
            end = k + 1;
            break;
          }
        }
        headerTokens.push_back(typeStr.substr(start, end - start));
        pos = end;
      }

      fieldType = parseBf3DrmtHeaderType(headerTokens, egglog);

    } else if (typeStr.find("bf3drmt_bits") == 0) {
      // p4hir_bits N
      size_t spacePos = typeStr.find(' ');
      if (spacePos != std::string::npos) {
        std::string widthStr = typeStr.substr(spacePos + 1);
        size_t parenPos = widthStr.find(')');
        if (parenPos != std::string::npos)
          widthStr = widthStr.substr(0, parenPos);

        unsigned width = static_cast<unsigned>(std::stoi(widthStr));
        std::string strType = "!bf3drmt.bit<" + std::to_string(width) + ">";
        fieldType = mlir::parseType(strType, &egglog.context);
      }
    } else {
      // Fallback: regular MLIR type
      fieldType = mlir::parseType(typeStr, &egglog.context);
    }

    if (fieldType) {
      fields.push_back(
          {mlir::StringAttr::get(&egglog.context, key), fieldType});
    }
  }

  mlir::DictionaryAttr annots = mlir::DictionaryAttr::get(&egglog.context, {});
  return mlir::edamlir::bf3drmt::StructType::get(&egglog.context, structName, fields, annots);
}

std::vector<std::string> stringifyBf3DrmtStructType(mlir::Type type, Egglog &egglog) {
    llvm::outs() << "[stringifyBf3DrmtStructType] type = " << type << "\n";
  std::vector<std::string> split;
  auto structType = type.cast<mlir::edamlir::bf3drmt::StructType>();

  split.push_back("bf3drmt_struct");

  // First, emit the struct name
  split.push_back("(NamedAttr \"name\" (StringAttr \"" +
                  structType.getName().str() + "\"))");

  // Then, emit fields
  for (auto field : structType.getFields()) {
    std::string fieldStr = "(NamedAttr \"" + field.name.str() +
                           "\" (TypeAttr ";

    if (auto hdrType =
            field.type.dyn_cast<mlir::edamlir::bf3drmt::HeaderType>()) {
      // Delegate header
      auto sub = stringifyBf3DrmtHeaderType(hdrType, egglog);
      fieldStr += "(" + llvm::join(sub, " ") + ")";
    } else if (auto bitsType =
                   field.type.dyn_cast<mlir::edamlir::bf3drmt::BitsType>()) {
      fieldStr += "(bf3drmt_bits " +
                  std::to_string(bitsType.getWidth()) + ")";
    } else {
      std::string typeStr;
      llvm::raw_string_ostream rso(typeStr);
      field.type.print(rso);
      rso.flush();
      fieldStr += typeStr;
    }

    fieldStr += "))";
    split.push_back(fieldStr);
  }

  return split;
}

/** Parse Bf3Drmt reference type (function p4hir_ref (Type) Type) */
mlir::Type parseBf3DrmtReferenceType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "bf3drmt_ref");

    // Parse inner type parameter
    std::string innerTypeStr = split[1];
    
    // Use egglog's parseType method like in parseRankedTensorType
    mlir::Type innerType = egglog.parseType(innerTypeStr);
    if (!innerType) {
        llvm::errs() << "Failed to parse inner type: " << innerTypeStr << "\n";
        return nullptr;
    }

    // Create reference type using P4HIR's RefType::get
    return mlir::edamlir::bf3drmt::ReferenceType::get(innerType);
}

/** Stringify P4HIR reference type */
std::vector<std::string> stringifyBf3DrmtReferenceType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    // Get the inner type from the reference type
    auto refType = type.cast<mlir::edamlir::bf3drmt::ReferenceType>();
    mlir::Type innerType = refType.getObjectType();

    // Get the string representation of the inner type using egglog's eggifyType
    std::string innerTypeStr = egglog.eggifyType(innerType);
    
    split.push_back("bf3drmt_ref");
    // Add the inner type WITHOUT extra parentheses
    split.push_back(innerTypeStr);  // Remove the extra parentheses

    return split;
}

std::vector<std::string> stringifyLLVMPointerType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    llvm::outs() << "Stringifying LLVM PointerType\n";
    
    auto ptrType = mlir::dyn_cast<mlir::LLVM::LLVMPointerType>(type);
    assert(ptrType && "Expected LLVM PointerType");
    
    split.push_back("llvm_ptr");
    
    // No parameters - just the type name
    
    return split;
}

mlir::Type parseLLVMPointerType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "llvm_ptr");
    
    // LLVM pointer type takes no parameters
    assert(split.size() == 1 && "LLVM pointer type takes no parameters");
    
    // Create LLVM pointer type with default address space (0)
    return mlir::LLVM::LLVMPointerType::get(&egglog.context);
}

std::vector<std::string> stringifyLLVMStructType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    auto structType = mlir::dyn_cast<mlir::LLVM::LLVMStructType>(type);
    assert(structType && "Expected LLVM StructType");
    
    split.push_back("llvm_struct");
    
    // Wrap struct name in quotes to handle dots
    split.push_back("\"" + structType.getName().str() + "\"");
    
    // Add body types
    if (!structType.isOpaque()) {
        auto bodyTypes = structType.getBody();
        for (mlir::Type bodyType : bodyTypes) {
            auto bodyStr = egglog.eggifyType(bodyType);
            split.push_back(bodyStr);
        }
    }
    
    return split;
}

mlir::Type parseLLVMStructType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "llvm_struct");
    
    assert(split.size() >= 4 && "LLVM struct type needs name, packed flag, and body count");
    
    std::string structName = split[1];
    bool isPacked = (split[2] == "1");
    size_t numBodyTypes = std::stoul(split[3]);
    
    assert(split.size() == 4 + numBodyTypes && "Mismatch in body type count");
    
    if (numBodyTypes == 0) {
        // Opaque or empty struct
        if (structName.empty()) {
            return mlir::LLVM::LLVMStructType::getLiteral(&egglog.context, {}, isPacked);
        } else {
            return mlir::LLVM::LLVMStructType::getOpaque(structName, &egglog.context);
        }
    }
    
    // Parse body types
    llvm::SmallVector<mlir::Type> bodyTypes;
    for (size_t i = 0; i < numBodyTypes; ++i) {
        std::string typeStr = split[4 + i];
        mlir::Type bodyType = mlir::parseType(typeStr, &egglog.context);
        assert(bodyType && "Failed to parse body type");
        bodyTypes.push_back(bodyType);
    }
    
    // Create struct type
    if (structName.empty()) {
        // Literal struct
        return mlir::LLVM::LLVMStructType::getLiteral(&egglog.context, bodyTypes, isPacked);
    } else {
        // Identified struct
        auto identifiedType = mlir::LLVM::LLVMStructType::getIdentified(&egglog.context, structName);
        if (identifiedType.isOpaque()) {
            auto result = identifiedType.setBody(bodyTypes, isPacked);
            assert(succeeded(result) && "Failed to set struct body");
        }
        return identifiedType;
    }
}

std::vector<std::string> stringifyLLVMArrayType(mlir::Type type, Egglog& egglog) {
    std::vector<std::string> split;
    
    auto arrayType = mlir::dyn_cast<mlir::LLVM::LLVMArrayType>(type);
    assert(arrayType && "Expected LLVM ArrayType");
    
    split.push_back("llvm_array");
    
    // Add array size
    split.push_back(std::to_string(arrayType.getNumElements()));
    
    // Add element type
    auto elementTypeStr = egglog.eggifyType(arrayType.getElementType());
    split.push_back(elementTypeStr);
    
    return split;
}

mlir::Type parseLLVMArrayType(const std::vector<std::string>& split, Egglog& egglog) {
    std::string typeType = split[0];
    assert(typeType == "llvm_array");
    
    assert(split.size() == 3 && "LLVM array type needs size and element type");
    
    uint64_t numElements = std::stoull(split[1]);
    mlir::Type elementType = egglog.parseType({split[2]});
    
    return mlir::LLVM::LLVMArrayType::get(elementType, numElements);
}