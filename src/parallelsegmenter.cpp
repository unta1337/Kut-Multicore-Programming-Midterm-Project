﻿#include "parallelsegmenter.h"
#include "lockutils.hpp"
#include "parallelfacegraph.h"
#include <unordered_set>

#include <chrono>
#include <color.hpp>
#include <glm/gtx/normal.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <iostream>
#include <omp.h>

ParallelSegmenter::ParallelSegmenter(TriangleMesh* mesh, float tolerance) : Segmenter(mesh, tolerance) {
}

inline glm::vec3 ParallelSegmenter::get_normal_key(std::unordered_map<glm::vec3, size_t, Vec3Hash>& count_map,
                                                   glm::vec3& normal, float tolerance) {
    tolerance = tolerance <= 0 ? this->tolerance : tolerance;
    for (const auto& entry : count_map) {
        glm::vec3 compare = entry.first;
        float norm_angle = glm::degrees(glm::angle(compare, normal));

        if (norm_angle < tolerance) {
            normal = compare;
            break;
        }
    }
    return normal;
}

inline void ParallelSegmenter::init_count_map(std::unordered_map<glm::vec3, size_t, Vec3Hash>& count_map,
                                              std::vector<glm::vec3>& face_normals) {
    std::vector<std::unordered_map<glm::vec3, size_t, Vec3Hash>*> sub_count_map_list(omp_get_max_threads());
    #pragma omp parallel
    {
        std::unordered_map<glm::vec3, size_t, Vec3Hash> sub_count_map;
        float half_tolerance = tolerance / 2;
        #pragma omp for
        for (int i = 0; i < face_normals.size(); i++) {
            glm::vec3 normal = get_normal_key(count_map, face_normals[i], half_tolerance);
            sub_count_map[normal]++;
        }
        sub_count_map_list[omp_get_thread_num()] = &sub_count_map;

        #pragma omp barrier
        #pragma omp single
        {
            int end = omp_get_num_threads();
            for (int i = 0; i < end; i++) {
                for (auto it = sub_count_map_list[i]->begin(); it != sub_count_map_list[i]->end(); it++) {
                    glm::vec3 normal = it->first;
                    count_map[get_normal_key(count_map, normal)] += it->second;
                }
            }
        }
    }
}

std::vector<TriangleMesh*> ParallelSegmenter::do_segmentation() {
    timer.onTimer(TIMER_TOTAL);
    STEP_LOG(std::cout << "[Begin] Preprocessing.\n");
    timer.onTimer(TIMER_PREPROCESSING);
    STEP_LOG(std::cout << "[Begin] Normal Vector Computation.\n");
    timer.onTimer(TIMER_NORMAL_VECTOR_COMPUTATION);

    // obj에 포함된 면의 개수만큼 법선 벡터 계산 필요.
    std::vector<glm::vec3> face_normals(mesh->index.size());

    // 오브젝트에 포함된 면에 대한 법선 벡터 계산.
    #pragma omp parallel for
    for (int i = 0; i < mesh->index.size(); i++) {
        glm::ivec3& index = mesh->index[i];
        face_normals[i] = glm::triangleNormal(mesh->vertex[index[0]], mesh->vertex[index[1]], mesh->vertex[index[2]]);
    }

    timer.offTimer(TIMER_NORMAL_VECTOR_COMPUTATION);
    STEP_LOG(std::cout << "[End] Normal Vector Computation.\n");

    STEP_LOG(std::cout << "[Begin] Map Count.\n");
    timer.onTimer(TIMER_MAP_COUNT);

    size_t face_normals_count = face_normals.size();

    // 법선 벡터 -> 개수.
    // 특정 법선 벡터와 비슷한 방향성을 갖는 법선 벡터의 개수.
    std::unordered_map<glm::vec3, size_t, Vec3Hash> count_map;
    init_count_map(count_map, face_normals);
    // 병렬을 위한 상호배제

    // 법선 벡터 -> 삼각형(면).
    // 특정 법선 벡터와 비슷한 방향성을 갖는 벡터를 법선 벡터로 갖는 면.
    std::unordered_map<glm::vec3, std::vector<Triangle>, Vec3Hash> normal_triangle_list_map;

    for (const auto& entry : count_map) {
        normal_triangle_list_map.insert({entry.first, std::vector<Triangle>()});
    }

    timer.offTimer(TIMER_MAP_COUNT);
    STEP_LOG(std::cout << "[End] Map Count. (Map size: " << count_map.size() << ")\n");

    STEP_LOG(std::cout << "[Begin] Normal Map Insertion.\n");
    timer.onTimer(TIMER_NORMAL_MAP_INSERTION);

    Vec3Hash hash_function;
    omp_lock_t* locks = new_locks(face_normals_count);

    #pragma omp parallel for
    for (int i = 0; i < face_normals_count; i++) {
        glm::vec3 target_normal = get_normal_key(count_map, face_normals[i]);
        size_t normal_hash = hash_function(target_normal) % face_normals_count;

        Triangle triangle;
        glm::ivec3 indexes = mesh->index[i];
        for (int d = 0; d < 3; d++) {
            triangle.vertex[d] = mesh->vertex[indexes[d]];
        }

        LOCK(locks, normal_hash, normal_triangle_list_map[target_normal].push_back(triangle));
    }

    timer.offTimer(TIMER_NORMAL_MAP_INSERTION);
    STEP_LOG(std::cout << "[End] Normal Map Insertion. (Total: " << normal_triangle_list_map.size() << ")\n");

    timer.offTimer(TIMER_PREPROCESSING);
    STEP_LOG(std::cout << "[End] Preprocessing.\n");

    STEP_LOG(std::cout << "[Begin] Connectivity Checking and Triangle Mesh Generating.\n");
    timer.onTimer(TIMER_CC_N_TMG);

    std::vector<TriangleMesh*> result;
    int number = 0;
    for (auto iter : normal_triangle_list_map) {
        STEP_LOG(std::cout << "[Step] FaceGraph: Init.\n");
        ParallelFaceGraph fg(&iter.second, &timer);

        STEP_LOG(std::cout << "[Step] FaceGraph: Get Segments.\n");
        std::vector<std::vector<Triangle>> segments = fg.get_segments();

        STEP_LOG(std::cout << "[Step] Triangle Mesh Generating.\n");
        timer.onTimer(TIMER_TRIANGLE_MESH_GENERATING);
        #pragma omp parallel for
        for (int i = 0; i < segments.size(); i++) {
            auto subs = segments[i];
            TriangleMesh* sub_object = triangle_list_to_obj(subs);
            sub_object->material->diffuse = glm::vec3(1, 0, 0);
            strcpy(sub_object->material->name, ("sub_materials_" + std::to_string(number)).c_str());
            strcpy(sub_object->name, (std::string(mesh->name) + "_seg_" + std::to_string(number++)).c_str());
            #pragma omp critical
            result.push_back(sub_object);
        }
        timer.offTimer(TIMER_TRIANGLE_MESH_GENERATING);
    }

    timer.offTimer(TIMER_CC_N_TMG);
    STEP_LOG(std::cout << "[End] Connectivity Checking and Triangle Mesh Generating.\n");

    STEP_LOG(std::cout << "[Begin] Segment Coloring.\n");
    timer.onTimer(TIMER_SEGMENT_COLORING);

    #pragma omp parallel for
    for (int i = 0; i < result.size(); i++) {
        result[i]->material->diffuse = Color::get_color_from_jet((float)i, 0, (float)result.size());
        result[i]->material->ambient = glm::vec3(1.0f, 1.0f, 1.0f);
        result[i]->material->specular = glm::vec3(0.5f, 0.5f, 0.5f);
    }

    STEP_LOG(std::cout << "[End] Segment Coloring.\n");
    timer.offTimer(TIMER_SEGMENT_COLORING);

    normal_triangle_list_map.clear();

    destroy_locks(locks, face_normals_count);

    timer.offTimer(TIMER_TOTAL);
    return result;
};
