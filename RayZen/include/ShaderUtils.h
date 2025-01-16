#include <fstream>
#include <sstream>
#include <string>

// Function to read file content into a string
std::string readFile(const std::string& filePath) {
    std::ifstream file(filePath);
    std::stringstream fileStream;
    fileStream << file.rdbuf();
    return fileStream.str();
}
