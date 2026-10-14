#include "DictionaryStore.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char *argv[])
{
    try {
        if ((argc == 2 && std::string(argv[1]) == "--activate") ||
            argc > 4) {
            throw std::invalid_argument(
                "usage: manage_keyword_index [index_root] or "
                "manage_keyword_index --activate GENERATION [index_root]");
        }
        const std::string root = argc == 2 || argc == 4
            ? argv[argc - 1] : "data/keyword_index";
        DictionaryGeneration generation;
        if (argc == 1 || argc == 2) {
            generation = DictionaryStore::loadCurrent(root);
        } else if ((argc == 3 || argc == 4) &&
                   std::string(argv[1]) == "--activate") {
            generation = DictionaryStore::activate(root, argv[2]);
        } else {
            throw std::invalid_argument(
                "usage: manage_keyword_index [index_root] or "
                "manage_keyword_index --activate GENERATION [index_root]");
        }
        std::cout << "Generation: " << generation.name << '\n'
                  << "English words: " << generation.repository->wordCount(0) << '\n'
                  << "Chinese words: " << generation.repository->wordCount(1) << '\n';
    } catch (const std::exception &error) {
        std::cerr << "keyword index: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
