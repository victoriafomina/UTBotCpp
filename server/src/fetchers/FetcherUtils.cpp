/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2012-2021. All rights reserved.
 */

#include "FetcherUtils.h"

#include <clang/Tooling/CompilationDatabase.h>

#include <memory>

using std::shared_ptr;

types::Type ParamsHandler::getType(const clang::QualType &paramDef,
                                   const clang::QualType &paramDecl) {
    const clang::QualType realParamType = paramDef.getCanonicalType();
    const std::string usedParamTypeString =
        paramDecl.getNonReferenceType().getUnqualifiedType().getAsString();
    return types::Type(realParamType, usedParamTypeString);
}

std::shared_ptr<types::FunctionInfo>
ParamsHandler::getFunctionPointerDeclaration(const clang::FunctionType *fType,
                                             const string &fName,
                                             const clang::SourceManager &mng,
                                             bool isArray) {
    auto functionParamDescription = std::make_shared<types::FunctionInfo>();
    functionParamDescription->name = fName;
    functionParamDescription->returnType = types::Type(fType->getReturnType().getCanonicalType(),
                                                       fType->getReturnType().getAsString());
    int paramCounter = 0;
    if (auto fProtoType = llvm::dyn_cast<clang::FunctionProtoType>(fType)) {
        for (const auto &ftParam : fProtoType->getParamTypes()) {
            functionParamDescription->params.push_back(
                { getType(ftParam, ftParam), "param" + std::to_string(++paramCounter) });
        }
    }
    functionParamDescription->isArray = isArray;
    return functionParamDescription;
}
ClangToolRunner::ClangToolRunner(
    std::shared_ptr<clang::tooling::CompilationDatabase> compilationDatabase)
    : compilationDatabase(std::move(compilationDatabase)) {
}

void ClangToolRunner::checkStatus(int status) const {
    LOG_IF_S(ERROR, status == 1) << "Error occurred while running clang tool";
    LOG_IF_S(ERROR, status == 2) << "Some files are skipped due to missing compile commands";
}
