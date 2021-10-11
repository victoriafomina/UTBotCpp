/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2012-2021. All rights reserved.
 */

#include "TypesResolver.h"

#include "Paths.h"
#include "clang-utils/ASTPrinter.h"
#include "fetchers/Fetcher.h"
#include "fetchers/FetcherUtils.h"
#include "utils/LogUtils.h"

#include <clang/AST/ASTContext.h>

#include "utils/path/FileSystemPath.h"
#include <vector>

TypesResolver::TypesResolver(const Fetcher *parent, utbot::Language srcLanguage) : parent(parent), srcLanguage(srcLanguage) {
}

static bool canBeReplaced(const string &nameInMap, const string &name) {
    return nameInMap.empty() && !name.empty();
}

template <class Info>
bool isCandidateToReplace(uint64_t id,
                          std::unordered_map<uint64_t, Info> &someMap,
                          std::string const &name) {
    auto iterator = someMap.find(id);
    if (iterator != someMap.end()) {
        std::string nameInMap = iterator->second.name;
        return canBeReplaced(nameInMap, name);
    }
    return true;
}

static size_t getRecordSize(const clang::RecordDecl *D) {
    return D->getASTContext().getTypeSize(D->getASTContext().getRecordType(D)) / 8;
}

static size_t getDeclAlignment(const clang::TagDecl *T) {
    return T->getASTContext().getTypeAlign(T->getTypeForDecl()) / 8;
}

template <class Info>
static void addInfo(uint64_t id, std::unordered_map<uint64_t, Info> &someMap, Info info) {
    auto [iterator, inserted] = someMap.emplace(id, info);
    LOG_IF_S(MAX, !inserted) << "Type with id=" << id << " already existed";
    if (!inserted) {
        std::string nameInMap = iterator->second.name;
        if (canBeReplaced(nameInMap, info.name)) {
            iterator->second = info;
            LOG_S(DEBUG) << "Replace unnamed type with typedef: " << info.name;
        } else if (!nameInMap.empty() && info.name.empty()) {
            LOG_S(MAX) << "Already replaced with typedef: " << nameInMap;
        } else if (nameInMap != info.name) {
            LOG_S(WARNING) << "Collision happened between: '" << nameInMap << "' and '" << info.name
                           << "'";
        }
    }
}

void TypesResolver::resolveStruct(const clang::RecordDecl *D, const std::string &name) {
    clang::ASTContext const &context = D->getASTContext();
    clang::SourceManager const &sourceManager = context.getSourceManager();

    clang::QualType canonicalType = context.getTypeDeclType(D).getCanonicalType();
    uint64_t id = types::Type::getIdFromCanonicalType(canonicalType);
    if (!isCandidateToReplace(id, parent->projectTypes->structs, name)) {
        return;
    }
    types::StructInfo structInfo;
    fs::path filename =
        sourceManager.getFilename(sourceManager.getSpellingLoc(D->getLocation())).str();
    structInfo.filePath = Paths::getCCJsonFileFullPath(filename, parent->buildRootPath);
    structInfo.name = name;


    if (Paths::isGtest(structInfo.filePath)) {
        return;
    }

    std::stringstream ss;
    structInfo.definition = ASTPrinter::getSourceText(D->getSourceRange(), sourceManager);
    ss << "Struct: " << structInfo.name << "\n"
       << "\tFile path: " << structInfo.filePath.string() << "";
    std::vector<types::Field> fields;
    for (const clang::FieldDecl *F : D->fields()) {
        types::Field field;
        field.name = F->getNameAsString();
        const clang::QualType paramType = F->getType().getCanonicalType();
        field.type = types::Type(paramType, paramType.getAsString());
        if (field.type.isPointerToFunction()) {
            structInfo.functionFields[field.name] = ParamsHandler::getFunctionPointerDeclaration(
                F->getFunctionType(), field.name, sourceManager,
                field.type.isArrayOfPointersToFunction());
            auto returnType = F->getFunctionType()->getReturnType();
            if (returnType->isPointerType() && returnType->getPointeeType()->isStructureType()) {
                fs::path sourceFilePath =
                    sourceManager.getFileEntryForID(sourceManager.getMainFileID())
                        ->tryGetRealPathName()
                        .str();
                string structName =
                    returnType->getPointeeType().getBaseTypeIdentifier()->getName().str();
                if (!CollectionUtils::containsKey((*parent->structsDeclared).at(sourceFilePath),
                                                  structName)) {
                    (*parent->structsToDeclare)[sourceFilePath].insert(structName);
                }
            }
        } else if (field.type.isArrayOfPointersToFunction()) {
            structInfo.functionFields[field.name] = ParamsHandler::getFunctionPointerDeclaration(
                F->getType()->getPointeeType()->getPointeeType()->getAs<clang::FunctionType>(),
                field.name, sourceManager, field.type.isArrayOfPointersToFunction());
        }
        field.size = context.getTypeSize(F->getType()) / 8;
        field.offset = context.getFieldOffset(F) / 8;
        if (LogUtils::isMaxVerbosity()) {
            ss << "\n\t" << field.type.typeName() << " " << field.name << ";";
        }
        fields.push_back(field);
    }
    structInfo.fields = fields;
    structInfo.size = getRecordSize(D);
    structInfo.alignment = getDeclAlignment(D);

    addInfo(id, parent->projectTypes->structs, structInfo);
    ss << "\nName: " << structInfo.name << ", id: " << id << " , size: " << structInfo.size << "\n";

    updateMaximumAlignment(structInfo.alignment);

    LOG_S(DEBUG) << ss.str();
}

static std::optional<string> getAccess(const clang::Decl *decl) {
    const clang::DeclContext *pContext = decl->getDeclContext();
    std::vector<string> result;
    while (pContext != nullptr) {
        if (auto pNamedDecl = llvm::dyn_cast<clang::NamedDecl>(pContext)) {
            auto name = pNamedDecl->getNameAsString();
            if (!name.empty()) {
                result.push_back(name);
            }
            pContext = pContext->getParent();
        } else {
            break;
        }
    }
    if (result.empty()) {
        return std::nullopt;
    }
    return StringUtils::joinWith(llvm::reverse(result), "::");
}

void TypesResolver::resolveEnum(const clang::EnumDecl *EN, const string &name) {
    clang::ASTContext const &context = EN->getASTContext();
    clang::SourceManager const &sourceManager = context.getSourceManager();

    clang::QualType canonicalType = context.getTypeDeclType(EN).getCanonicalType();
    uint64_t id = types::Type::getIdFromCanonicalType(canonicalType);
    if (!isCandidateToReplace(id, parent->projectTypes->enums, name)) {
        return;
    }
    types::EnumInfo enumInfo;
    enumInfo.name = name;
    enumInfo.filePath = Paths::getCCJsonFileFullPath(
        sourceManager.getFilename(EN->getLocation()).str(), parent->buildRootPath.string());
    clang::QualType promotionType = EN->getPromotionType();
    enumInfo.size = context.getTypeSize(promotionType) / 8;

    enumInfo.access = getAccess(EN);

    for (auto it = EN->enumerator_begin(); it != EN->enumerator_end(); ++it) {
        types::EnumInfo::EnumEntry enumEntry;
        string entryName = it->getNameAsString();
        enumEntry.name = entryName;
        enumEntry.value = std::to_string(it->getInitVal().getSExtValue());
        enumInfo.valuesToEntries[enumEntry.value] = enumEntry;
        enumInfo.namesToEntries[enumEntry.name] = enumEntry;
    }
    enumInfo.definition = ASTPrinter::getSourceText(EN->getSourceRange(), sourceManager);
    enumInfo.alignment = getDeclAlignment(EN);

    addInfo(id, parent->projectTypes->enums, enumInfo);
    LOG_S(DEBUG) << "\nName: " << enumInfo.name << ", id: " << id << "\n";

    updateMaximumAlignment(enumInfo.alignment);

    std::stringstream ss;
    ss << "EnumInfo: " << enumInfo.name << "\n"
       << "\tFile path: " << enumInfo.filePath.string();
    LOG_S(DEBUG) << ss.str();
}
void TypesResolver::updateMaximumAlignment(uint64_t alignment) const {
    uint64_t &maximumAlignment = *(this->parent->maximumAlignment);
    maximumAlignment = std::max(maximumAlignment, alignment);
}

void TypesResolver::resolveUnion(const clang::RecordDecl *D, const std::string &name) {
    clang::ASTContext const &context = D->getASTContext();
    clang::SourceManager const &sourceManager = context.getSourceManager();

    clang::QualType canonicalType = context.getTypeDeclType(D).getCanonicalType();
    uint64_t id = types::Type::getIdFromCanonicalType(canonicalType);
    if (!isCandidateToReplace(id, parent->projectTypes->unions, name)) {
        return;
    }
    types::UnionInfo unionInfo;
    unionInfo.filePath = Paths::getCCJsonFileFullPath(
        sourceManager.getFilename(D->getLocation()).str(), parent->buildRootPath.string());
    unionInfo.name = name;

    if (Paths::isGtest(unionInfo.filePath)) {
        return;
    }

    std::stringstream ss;
    unionInfo.definition = ASTPrinter::getSourceText(D->getSourceRange(), sourceManager);
    ss << "Union: " << unionInfo.name << "\n"
       << "\tFile path: " << unionInfo.filePath.string() << "";
    std::vector<types::Field> fields;
    for (const clang::FieldDecl *F : D->fields()) {
        types::Field field;
        string fieldName = F->getNameAsString();
        field.name = fieldName;
        const clang::QualType paramType = F->getType().getCanonicalType();
        field.type = types::Type(paramType, paramType.getAsString());
        // TODO: add flag in logger that prevents line ending
        if (LogUtils::isMaxVerbosity()) {
            ss << "\n\t" << field.type.typeName() << " " << field.name << ";";
        }
        fields.push_back(field);
    }
    unionInfo.fields = fields;
    unionInfo.size = getRecordSize(D);
    unionInfo.alignment = getDeclAlignment(D);

    addInfo(id, parent->projectTypes->unions, unionInfo);
    ss << "\nName: " << unionInfo.name << ", id: " << id << "\n";

    updateMaximumAlignment(unionInfo.alignment);

    LOG_S(DEBUG) << ss.str();
}

void TypesResolver::resolve(const clang::QualType &type) {
    clang::TagDecl *tagDecl = type->getAsTagDecl();
    if (tagDecl == nullptr) {
        return;
    }
    string name = tagDecl->getNameAsString();
    if (auto enumDecl = llvm::dyn_cast<clang::EnumDecl>(tagDecl)) {
        resolveEnum(enumDecl, name);
    }
    if (auto recordDecl = llvm::dyn_cast<clang::RecordDecl>(tagDecl)) {
        if (recordDecl->isStruct() || recordDecl->isClass()) {
            resolveStruct(recordDecl, name);
        }
        if (recordDecl->isUnion()) {
            resolveUnion(recordDecl, name);
        }
    }
}
