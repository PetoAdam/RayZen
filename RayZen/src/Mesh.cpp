#include "Mesh.h"
#include <fstream>
#include <sstream>
#include <iostream>

bool Mesh::loadFromOBJ(const std::string& filename, int materialIndex) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open OBJ file: " << filename << std::endl;
        return false;
    }
    
    std::vector<glm::vec3> vertices;
    std::string line;
    while (std::getline(file, line)) {
        // Parse vertex lines (e.g., "v 0.0 1.0 0.0")
        if (line.substr(0, 2) == "v ") {
            std::istringstream iss(line.substr(2));
            glm::vec3 v;
            iss >> v.x >> v.y >> v.z;
            vertices.push_back(v);
        }
        // Parse face lines (supports tokens like "3//1" by splitting on '/')
        else if (line.substr(0, 2) == "f ") {
            std::istringstream iss(line.substr(2));
            std::vector<unsigned int> vertexIndices;
            std::string token;
            while (iss >> token) {
                size_t pos = token.find('/');
                std::string indexStr = (pos == std::string::npos) ? token : token.substr(0, pos);
                unsigned int index = std::stoi(indexStr);
                vertexIndices.push_back(index);
            }
            // Triangulate the face (supports both triangles and polygons)
            if (vertexIndices.size() >= 3) {
                for (size_t i = 1; i < vertexIndices.size() - 1; ++i) {
                    Triangle tri;
                    // OBJ indices are 1-based.
                    tri.v0 = vertices[vertexIndices[0] - 1];
                    tri.v1 = vertices[vertexIndices[i] - 1];
                    tri.v2 = vertices[vertexIndices[i + 1] - 1];
                    tri.materialIndex = materialIndex;
                    triangles.push_back(tri);
                }
            }
        }
    }
    std::cout << "Loaded " << triangles.size() << " triangles." << std::endl;
    return true;
}
