// Copyright (C) 2018-2022 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h
#include "nbl/asset/utils/IShaderCompiler.h"
#include "nbl/asset/utils/shadercUtils.h"
#include "nbl/asset/utils/CGLSLVirtualTexturingBuiltinIncludeGenerator.h"
#include "nbl/asset/utils/shaderCompiler_serialization.h"
#include "nbl/core/xxHash256.h"

#include <sstream>
#include <regex>
#include <iterator>

using namespace nbl;
using namespace nbl::asset;

IShaderCompiler::IShaderCompiler(core::smart_refctd_ptr<system::ISystem>&& system)
    : m_system(std::move(system))
{
    m_defaultIncludeFinder = core::make_smart_refctd_ptr<CIncludeFinder>(core::smart_refctd_ptr(m_system));
    m_defaultIncludeFinder->addGenerator(core::make_smart_refctd_ptr<asset::CGLSLVirtualTexturingBuiltinIncludeGenerator>());
    m_defaultIncludeFinder->getIncludeStandard("", "nbl/builtin/glsl/utils/common.glsl");
}

std::string IShaderCompiler::preprocessShader(
    system::IFile* sourcefile,
    IShader::E_SHADER_STAGE stage,
    const SPreprocessorOptions& preprocessOptions,
    std::vector<CCache::SEntry::SPreprocessingDependency>* dependencies) const
{
    std::string code(sourcefile->getSize(), '\0');

    system::IFile::success_t success;
    sourcefile->read(success, code.data(), 0, sourcefile->getSize());
    if (!success)
        return nullptr;

    return preprocessShader(std::move(code), stage, preprocessOptions, dependencies);
}

auto IShaderCompiler::IIncludeGenerator::getInclude(const std::string& includeName) const -> IIncludeLoader::found_t
{
    core::vector<std::pair<std::regex, HandleFunc_t>> builtinNames = getBuiltinNamesToFunctionMapping();
    for (const auto& pattern : builtinNames)
    if (std::regex_match(includeName,pattern.first))
    {
        if (auto contents=pattern.second(includeName); !contents.empty())
        {
            // Welcome, you've came to a very disused piece of code, please check the first parameter (path) makes sense!
            _NBL_DEBUG_BREAK_IF(true);
            return {includeName,contents};
        }
    }

    return {};
}

core::vector<std::string> IShaderCompiler::IIncludeGenerator::parseArgumentsFromPath(const std::string& _path)
{
    core::vector<std::string> args;

    std::stringstream ss{ _path };
    std::string arg;
    while (std::getline(ss, arg, '/'))
        args.push_back(std::move(arg));

    return args;
}

IShaderCompiler::CFileSystemIncludeLoader::CFileSystemIncludeLoader(core::smart_refctd_ptr<system::ISystem>&& system) : m_system(std::move(system))
{}

auto IShaderCompiler::CFileSystemIncludeLoader::getInclude(const system::path& searchPath, const std::string& includeName) const -> found_t
{
    system::path path = searchPath / includeName;
    if (std::filesystem::exists(path))
        path = std::filesystem::canonical(path);

    core::smart_refctd_ptr<system::IFile> f;
    {
        system::ISystem::future_t<core::smart_refctd_ptr<system::IFile>> future;
        m_system->createFile(future, path.c_str(), system::IFile::ECF_READ);
        if (!future.wait())
            return {};
        future.acquire().move_into(f);
    }
    if (!f)
        return {};
    const size_t size = f->getSize();

    std::string contents(size, '\0');
    system::IFile::success_t succ;
    f->read(succ, contents.data(), 0, size);
    const bool success = bool(succ);
    assert(success);

    return {f->getFileName(),std::move(contents)};
}

IShaderCompiler::CIncludeFinder::CIncludeFinder(core::smart_refctd_ptr<system::ISystem>&& system) 
    : m_defaultFileSystemLoader(core::make_smart_refctd_ptr<CFileSystemIncludeLoader>(std::move(system)))
{
    addSearchPath("", m_defaultFileSystemLoader);
}

// ! includes within <>
// @param requestingSourceDir: the directory where the incude was requested
// @param includeName: the string within <> of the include preprocessing directive
// @param 
auto IShaderCompiler::CIncludeFinder::getIncludeStandard(const system::path& requestingSourceDir, const std::string& includeName) const -> IIncludeLoader::found_t
{
    IShaderCompiler::IIncludeLoader::found_t retVal;
    if (auto contents = tryIncludeGenerators(includeName))
        retVal = std::move(contents);
    else if (auto contents = trySearchPaths(includeName))
            retVal = std::move(contents);
    else retVal = m_defaultFileSystemLoader->getInclude(requestingSourceDir.string(), includeName);

    retVal.hash = nbl::core::XXHash_256((uint8_t*)(retVal.contents.data()), retVal.contents.size() * (sizeof(char) / sizeof(uint8_t)));
    return retVal;
}

// ! includes within ""
// @param requestingSourceDir: the directory where the incude was requested
// @param includeName: the string within "" of the include preprocessing directive
auto IShaderCompiler::CIncludeFinder::getIncludeRelative(const system::path& requestingSourceDir, const std::string& includeName) const -> IIncludeLoader::found_t
{
    IShaderCompiler::IIncludeLoader::found_t retVal;
    if (auto contents = m_defaultFileSystemLoader->getInclude(requestingSourceDir.string(),includeName))
        retVal = std::move(contents);
    else retVal = std::move(trySearchPaths(includeName));
    retVal.hash = nbl::core::XXHash_256((uint8_t*)(retVal.contents.data()), retVal.contents.size() * (sizeof(char) / sizeof(uint8_t)));
    return retVal;
}

void IShaderCompiler::CIncludeFinder::addSearchPath(const std::string& searchPath, const core::smart_refctd_ptr<IIncludeLoader>& loader)
{
    if (!loader)
        return;
    m_loaders.push_back(LoaderSearchPath{ loader, searchPath });
}

void IShaderCompiler::CIncludeFinder::addGenerator(const core::smart_refctd_ptr<IIncludeGenerator>& generatorToAdd)
{
    if (!generatorToAdd)
        return;

    // this will find the place of first generator with prefix <= generatorToAdd or end
    auto found = std::lower_bound(m_generators.begin(), m_generators.end(), generatorToAdd->getPrefix(),
        [](const core::smart_refctd_ptr<IIncludeGenerator>& generator, const std::string_view& value)
        {
            auto element = generator->getPrefix();
            return element.compare(value) > 0; // first to return false is lower_bound -> first element that is <= value
        });

    m_generators.insert(found, generatorToAdd);
}

auto IShaderCompiler::CIncludeFinder::trySearchPaths(const std::string& includeName) const -> IIncludeLoader::found_t
{
    for (const auto& itr : m_loaders)
    if (auto contents = itr.loader->getInclude(itr.searchPath,includeName))
        return contents;
    return {};
}

auto IShaderCompiler::CIncludeFinder::tryIncludeGenerators(const std::string& includeName) const -> IIncludeLoader::found_t
{
    // Need custom function because std::filesystem doesn't consider the parameters we use after the extension like CustomShader.hlsl/512/64
    auto removeExtension = [](const std::string& str)
    {
        return str.substr(0, str.find_last_of('.'));
    };

    auto standardizePrefix = [](const std::string_view& prefix) -> std::string
    {
        std::string ret(prefix);
        // Remove Trailing '/' if any, to compare to filesystem paths
        if (*ret.rbegin() == '/' && ret.size() > 1u)
            ret.resize(ret.size() - 1u);
        return ret;
    };

    auto extension_removed_path = system::path(removeExtension(includeName));
    system::path path = extension_removed_path.parent_path();

    // Try Generators with Matching Prefixes:
    // Uses a "Path Peeling" method which goes one level up the directory tree until it finds a suitable generator
    auto end = m_generators.begin();
    while (!path.empty() && path.root_name().empty() && end != m_generators.end())
    {
        auto begin = std::lower_bound(end, m_generators.end(), path.string(),
            [&standardizePrefix](const core::smart_refctd_ptr<IIncludeGenerator>& generator, const std::string& value)
            {
                const auto element = standardizePrefix(generator->getPrefix());
                return element.compare(value) > 0; // first to return false is lower_bound -> first element that is <= value
            });

        // search from new beginning to real end
        end = std::upper_bound(begin, m_generators.end(), path.string(),
            [&standardizePrefix](const std::string& value, const core::smart_refctd_ptr<IIncludeGenerator>& generator)
            {
                const auto element = standardizePrefix(generator->getPrefix());
                return value.compare(element) > 0; // first to return true is upper_bound -> first element that is < value
            });

        for (auto generatorIt = begin; generatorIt != end; generatorIt++)
        {
            if (auto contents = (*generatorIt)->getInclude(includeName))
                return contents;
        }

        path = path.parent_path();
    }

    return {};
}

core::smart_refctd_ptr<asset::ICPUShader> IShaderCompiler::CCache::find(const SEntry& mainFile, const IShaderCompiler::CIncludeFinder* finder) const
{
    auto foundRange = m_container.equal_range(mainFile);
    for (auto& found = foundRange.first; found != foundRange.second; found++)
    {
        bool allDependenciesMatch = true;
        // go through all dependencies
        for (auto i = 0; i < found->dependencies.size(); i++)
        {
            const auto& dependency = found->dependencies[i];

            IIncludeLoader::found_t header;
            if (dependency.standardInclude)
                header = finder->getIncludeStandard(dependency.requestingSourceDir, dependency.identifier);
            else
                header = finder->getIncludeRelative(dependency.requestingSourceDir, dependency.identifier);

            if (header.hash != dependency.hash || header.contents != dependency.contents)
            {
                allDependenciesMatch = false;
                break;
            }
        }
        if (allDependenciesMatch) {
            return found->value;
        }
    }
    return nullptr;
}

std::vector<uint8_t> IShaderCompiler::CCache::serialize()
{
    std::vector<uint8_t> shadersBuffer;
    json entries;
    json shaderCreationParams;
    for (auto& entry : m_container) {
        // Add the entry as a json array
        entries.push_back(entry);

        // Now create the CPU shader creation parameters struct
        CPUShaderCreationParams params;
        params.stage = entry.value->getStage();
        params.contentType = entry.value->getContentType();
        params.filepathHint = entry.value->getFilepathHint();
        params.offset = shadersBuffer.size();
        params.codeByteSize = entry.value->getContent()->getSize();

        // And add it to the shader creation parameters array
        shaderCreationParams.push_back(params);

        // Finally, insert the shader bytecode into the shaders buffer
        shadersBuffer.reserve(shadersBuffer.size() + params.codeByteSize);
        shadersBuffer.insert(shadersBuffer.end(), (uint8_t*)entry.value->getContent()->getPointer(), (uint8_t*)entry.value->getContent()->getPointer() + params.codeByteSize);
    }
    json containerJson{
        { "entries", entries },
        { "shaderCreationParams", shaderCreationParams },
    };
    std::string dumpedContainerJson = std::move(containerJson.dump());
    uint64_t dumpedContainerJsonLength = dumpedContainerJson.size();
    std::vector<uint8_t> retVal;
    retVal.reserve(CONTAINER_JSON_SIZE_BYTES + dumpedContainerJsonLength + shadersBuffer.size());
    // first CONTAINER_JSON_SIZE_BYTES (8) entries are the size of the json
    retVal.insert(retVal.end(), reinterpret_cast<uint8_t*>(&dumpedContainerJsonLength), reinterpret_cast<uint8_t*>(&dumpedContainerJsonLength) + CONTAINER_JSON_SIZE_BYTES);
    retVal.insert(retVal.end(), std::make_move_iterator(dumpedContainerJson.begin()), std::make_move_iterator(dumpedContainerJson.end()));
    retVal.insert(retVal.end(), std::make_move_iterator(shadersBuffer.begin()), std::make_move_iterator(shadersBuffer.end()));
    return retVal;
}

core::smart_refctd_ptr<IShaderCompiler::CCache> IShaderCompiler::CCache::deserialize(const std::span<const uint8_t> serializedCache)
{
    auto retVal = core::make_smart_refctd_ptr<CCache>();

    // First get the size of the json in the buffer, stored in the first 8 bytes
    const uint64_t* cacheStart = reinterpret_cast<const uint64_t*>(serializedCache.data());
    uint64_t containerJsonSize = cacheStart[0];
    // Next up get the json that stores the container data
    std::span<const char> cacheAsChar = { reinterpret_cast<const char*>(serializedCache.data()), serializedCache.size() };
    std::string_view containerJsonString(cacheAsChar.begin() + CONTAINER_JSON_SIZE_BYTES, cacheAsChar.begin() + CONTAINER_JSON_SIZE_BYTES + containerJsonSize);
    json containerJson = json::parse(containerJsonString);
    
    // Now retrieve two vectors, one with the entries and one with the extra data to recreate the CPUShaders
    std::vector<SEntry> entries;
    std::vector<CPUShaderCreationParams> shaderCreationParams;
    containerJson.at("entries").get_to(entries);
    containerJson.at("shaderCreationParams").get_to(shaderCreationParams);

    // We must now recreate the shaders, add them to each entry, then move the entry into the multiset
    for (auto i = 0u; i < entries.size(); i++) {
        // Create buffer to hold the code
        auto code = core::make_smart_refctd_ptr<ICPUBuffer>(shaderCreationParams[i].codeByteSize);
        // Copy the shader bytecode into the buffer
        memcpy(code->getPointer(), serializedCache.data() + CONTAINER_JSON_SIZE_BYTES + containerJsonSize + shaderCreationParams[i].offset, shaderCreationParams[i].codeByteSize);
        // Create the ICPUShader
        auto value = core::make_smart_refctd_ptr<ICPUShader>(std::move(code), shaderCreationParams[i].stage, shaderCreationParams[i].contentType, std::move(shaderCreationParams[i].filepathHint));
       
        entries[i].value = std::move(value);

        retVal->insert(std::move(entries[i]));
    }

    return retVal;
}

core::smart_refctd_ptr<IShaderCompiler::CCache> IShaderCompiler::CCache::deserialize(core::smart_refctd_ptr<const system::IFile> serializedCache) {
    const void* fileBufferPointer = serializedCache->getMappedPointer();
    assert(fileBufferPointer); // Check the file is readable
    
    size_t fileSize = serializedCache->getSize();
    std::span cacheSpan{ reinterpret_cast<const uint8_t*>(fileBufferPointer), fileSize };
    return IShaderCompiler::CCache::deserialize(std::span{ reinterpret_cast<const uint8_t*>(fileBufferPointer), fileSize });
}
