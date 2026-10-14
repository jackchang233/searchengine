#include "webpage/WebIndexStore.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    if (argc > 2) {
        std::cerr << "usage: " << argv[0] << " [index_root]\n";
        return 2;
    }
    const std::string root = argc == 2 ? argv[1] : "data/web_index";
    try {
        const WebIndexSnapshot snapshot = WebIndexStore::loadCurrent(root);
        std::size_t postings = 0;
        for (const auto &entry : snapshot.invertedIndex()) {
            postings += entry.second.size();
        }
        std::cout << "Generation: " << snapshot.generation() << '\n'
                  << "Documents: " << snapshot.offsets().size() << '\n'
                  << "Terms: " << snapshot.invertedIndex().size() << '\n'
                  << "Postings: " << postings << '\n';
    } catch (const std::exception &error) {
        std::cerr << "web index validation failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
