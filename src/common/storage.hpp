// storage.hpp
// Helper method delarations for filesystem shenanigans.

#pragma once

#include <filesystem>
#include <fstream>

namespace Common {
    namespace Storage {

        /**
         * @brief Base path for corpus files directory.
         * 
         */
        const std::filesystem::path CORPUS_BASE_PATH = "storage/corpus/";

        /**
         * @brief Base path for the record files directory.
         * 
         */
        const std::filesystem::path RECORDS_BASE_PATH = "storage/records/";

        /**
         * @brief Returns if a file at the specified path exists.
         * 
         * @param path 
         * @return true 
         * @return false 
         */
        bool FileExists(const std::filesystem::path &path);

        /**
         * @brief Returns if a file at the specified path is empty.
         * 
         * @param path 
         * @return true 
         * @return false 
         */
        bool IsFileEmpty(const std::filesystem::path &path);

        /**
         * @brief Deletes the file at the specified directory.
         * 
         * @param path 
         */
        void DeleteFile(const std::filesystem::path &path);

        /**
         * @brief Deletes the specified directory and it's contents recursively.
         * 
         * @param path 
         */
        void DeleteDirectory(const std::filesystem::path &path);

        /**
         * @brief Creates the specified directory.
         * 
         * @param path 
         * @return true 
         * @return false 
         */
        bool CreateDirectory(const std::filesystem::path &path);

        /**
         * @brief Creates file if it does not exist and opens it in append mode. Option for binary open.
         * 
         * @param basePath 
         * @param fileName 
         * @param binaryMode 
         * @return std::ofstream 
         */
        std::ofstream OpenFile_App(const std::filesystem::path &basePath, const std::string &fileName, const bool &binaryMode = false);

    }
}