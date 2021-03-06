#include "b_spline.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkDataObject.h"
#include "vtkFloatArray.h"
#include "vtkIdTypeArray.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPointSet.h"
#include "vtkPolyData.h"
#include "vtkSmartPointer.h"
#include "vtkStreamingDemandDrivenPipeline.h"

#include "Eigen/Dense"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

vtkStandardNewMacro(b_spline);

b_spline::b_spline()
{
    this->SetNumberOfInputPorts(2);
    this->SetNumberOfOutputPorts(1);
}

int b_spline::FillInputPortInformation(int port, vtkInformation* info)
{
    if (port == 0)
    {
        info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkPointSet");
        return 1;
    }
    else if (port == 1)
    {
        info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkPointSet");
        info->Set(vtkAlgorithm::INPUT_IS_OPTIONAL(), 1);
        return 1;
    }

    return 0;
}

int b_spline::RequestInformation(vtkInformation*, vtkInformationVector**, vtkInformationVector* output_vector)
{
    return 1;
}

int b_spline::RequestData(vtkInformation*, vtkInformationVector** input_vector, vtkInformationVector* output_vector)
{
    // Get access to information and data
    auto in_info = input_vector[0]->GetInformationObject(0);
    auto input = vtkPointSet::SafeDownCast(in_info->Get(vtkDataObject::DATA_OBJECT()));

    // Get output
    auto out_info = output_vector->GetInformationObject(0);
    auto output = vtkPolyData::SafeDownCast(out_info->Get(vtkDataObject::DATA_OBJECT()));

    // Get parameters
    if (this->Degree < 1)
    {
        std::cerr << "The B-spline degree must be at least 1" << std::endl;
        return 0;
    }

    if (this->NumberOfPoints < this->Degree + 1)
    {
        std::cerr << "The number of output points must be larger the B-spline degree" << std::endl;
        return 0;
    }

    if (input->GetNumberOfPoints() < this->Degree + 1)
    {
        std::cerr << "Number of de Boor points must be larger than the B-spline degree" << std::endl;
        return 0;
    }

    const auto degree = static_cast<std::size_t>(this->Degree);
    const auto hit_endpoints = this->HitEndpoints != 0;
    const auto num_output_points = static_cast<std::size_t>(this->NumberOfPoints);

    // Get all de Boor points
    const auto num_de_boor_points = static_cast<std::size_t>(input->GetPoints()->GetNumberOfPoints());

    std::vector<Eigen::Vector3d> de_boor_points(num_de_boor_points);

    for (std::size_t i = 0; i < num_de_boor_points; ++i)
    {
        input->GetPoints()->GetPoint(i, de_boor_points[i].data());
    }

    std::cout << "Number of input points: " << num_de_boor_points << std::endl;

    // Create knot vector
    std::vector<double> knot_vector(num_de_boor_points + degree + 1);

    auto first = knot_vector.begin();
    auto last = knot_vector.end();

    if (hit_endpoints)
    {
        // Use multiplicity of the first and last de Boor point
        for (std::size_t i = 0; i < degree; ++i)
        {
            knot_vector[i] = 0.0;
            knot_vector[knot_vector.size() - 1 - i] = static_cast<double>(num_de_boor_points - degree);
        }

        // Adjust iterators
        first += degree;
        last -= degree;
    }

    std::iota(first, last, 0);

    std::cout << "Knot vector: " << print_collection(knot_vector.begin(), knot_vector.end()) << std::endl;

    // Create derivatives of the B-Spline
    const auto de_boor_points_first_derivative = derive(de_boor_points, knot_vector.cbegin(), knot_vector.cend(), degree);
    const auto de_boor_points_second_derivative = derive(de_boor_points_first_derivative, knot_vector.cbegin() + 1, knot_vector.cend() - 1, degree - 1);

    // Compute position at output points, with additional information
    const auto u_begin = knot_vector[degree];
    const auto u_end = knot_vector[num_de_boor_points];
    const auto u_step = (u_end - u_begin) / (num_output_points - 1);

    std::vector<Eigen::Vector3d> points(num_output_points);
    std::vector<double> points_arc_position(num_output_points);

    std::vector<Eigen::Vector3d> tangent((degree > 2) ? num_output_points : 0);
    std::vector<Eigen::Vector3d> binormal((degree > 2) ? num_output_points : 0);
    std::vector<Eigen::Vector3d> normal((degree > 2) ? num_output_points : 0);

    for (std::size_t i = 0; i < num_output_points; ++i)
    {
        auto u = u_begin + i * u_step;

        points[i] = compute_point(de_boor_points, knot_vector.cbegin(), knot_vector.cend(), this->Degree, u);
        points_arc_position[i] = u;

        if (degree > 2)
        {
            tangent[i] = compute_point(de_boor_points_first_derivative, knot_vector.cbegin() + 1, knot_vector.cend() - 1, this->Degree - 1, u).normalized();
            binormal[i] = tangent[i].cross(compute_point(de_boor_points_second_derivative, knot_vector.cbegin() + 2, knot_vector.cend() - 2, this->Degree - 2, u)).normalized();
            normal[i] = tangent[i].cross(binormal[i]);
        }
    }

    // Calculate distance from the point to the output points
    if (input_vector[1] != nullptr && input_vector[1]->GetInformationObject(0) != nullptr)
    {
        auto in_point_info = input_vector[1]->GetInformationObject(0);
        auto input_point = vtkPointSet::SafeDownCast(in_point_info->Get(vtkDataObject::DATA_OBJECT()));

        if (input_point->GetPoints()->GetNumberOfPoints() == 1)
        {
            double nearest_arc;
            double nearest_distance;

            // Get point
            Eigen::Vector3d point;
            input_point->GetPoints()->GetPoint(0, point.data());

            // Use middle of each segment as starting point
            std::vector<std::pair<double, double>> distances(num_de_boor_points - degree);

            auto comparator = [](const std::pair<double, double>& lhs, const std::pair<double, double>& rhs) -> bool
            {
                return lhs.second < rhs.second;
            };

            const double delta = knot_vector[degree + 1] - knot_vector[degree];

            std::transform(knot_vector.begin() + degree, knot_vector.begin() + num_de_boor_points, distances.begin(),
                [this, &de_boor_points, &knot_vector, &point, delta](double u) -> std::pair<double, double>
                {
                    return std::make_pair(u + 0.5, (point - compute_point(de_boor_points, knot_vector.cbegin(), knot_vector.cend(), this->Degree, u + 0.5 * delta)).norm());
                });

            std::cout << "Initial guess: " << print_collection(distances.begin(), distances.end()) << std::endl;

            if (degree == 2)
            {
                // Try to find a local minimum by finding the roots of the derivative of the distance polynomial
                // This means, we find the minimum by looking for the root of the derivative
                for (auto distance_it = distances.begin(); distance_it != distances.end(); ++distance_it)
                {
                    // Get index
                    const auto index = degree + static_cast<std::size_t>(std::floor((distance_it->first - u_begin) / delta));

                    // Get analytical derivative...
                    // ... calculate denominators
                    const double alpha =
                        +knot_vector[index + 2] * knot_vector[index + 1]
                        - knot_vector[index + 2] * knot_vector[index + 0]
                        - knot_vector[index + 1] * knot_vector[index + 0]
                        + knot_vector[index + 0] * knot_vector[index + 0];

                    const double gamma =
                        +knot_vector[index + 1] * knot_vector[index + 1]
                        - knot_vector[index + 1] * knot_vector[index + 0]
                        - knot_vector[index + 1] * knot_vector[index - 1]
                        + knot_vector[index + 0] * knot_vector[index - 1];

                    // ... bring B-Spline into the form au� + bu + c
                    const auto a = (
                        +alpha * de_boor_points[index - 2]
                        - alpha * de_boor_points[index - 1]
                        - gamma * de_boor_points[index - 1]
                        + gamma * de_boor_points[index - 0]) / (alpha * gamma);

                    const auto b = (
                        -2.0 * alpha * knot_vector[index + 1] * de_boor_points[index - 2]
                        + alpha * knot_vector[index + 1] * de_boor_points[index - 1]
                        + alpha * knot_vector[index - 1] * de_boor_points[index - 1]
                        + gamma * knot_vector[index + 2] * de_boor_points[index - 1]
                        + gamma * knot_vector[index + 0] * de_boor_points[index - 1]
                        - 2.0 * gamma * knot_vector[index + 0] * de_boor_points[index - 0]) / (alpha * gamma);

                    const auto c = (
                        +alpha * knot_vector[index + 1] * knot_vector[index + 1] * de_boor_points[index - 2]
                        - alpha * knot_vector[index + 1] * knot_vector[index - 1] * de_boor_points[index - 1]
                        - gamma * knot_vector[index + 2] * knot_vector[index + 0] * de_boor_points[index - 1]
                        + gamma * knot_vector[index + 0] * knot_vector[index + 0] * de_boor_points[index - 0]) / (alpha * gamma);

                    // ... create polynomial relative to p by setting (s(u) - p) as au� + bu + c_dist
                    const auto c_dist = c - point;

                    // ... create distance polynomial of form vu^4 + wu^3 + xu^2 + yu + z as (s(u) - p)*(s(u) - p)
                    const auto v = a.dot(a);
                    const auto w = 2.0 * a.dot(b);
                    const auto x = 2.0 * a.dot(c_dist) + b.dot(b);
                    const auto y = 2.0 * b.dot(c_dist);
                    const auto z = c_dist.dot(c_dist);

                    // ... calculate first derivative of form 4vu^3 + 3wu^2 + 2xu + y = v'u^3 + w'u^2 + x'u + y'
                    const auto v_dash = 4.0 * v;
                    const auto w_dash = 3.0 * w;
                    const auto x_dash = 2.0 * x;
                    const auto y_dash = 1.0 * y;

                    // ... calculate second derivative of form 12vu^2 + 6wu + 2x = v''u^2 + w''u + x''
                    const auto v_dash_dash = 12.0 * v;
                    const auto w_dash_dash = 6.0 * w;
                    const auto x_dash_dash = 2.0 * x;

                    // Use Newton's method to find the root of the first derivative
                    auto u = distance_it->first;

                    const auto u_left = u - 0.5 * delta;
                    const auto u_right = u + 0.5 * delta;

                    for (std::size_t i = 0; i < 10; ++i)
                    {
                        // Calculate derivatives by inserting u
                        const auto first_derivative = v_dash * std::pow(u, 3.0) + w_dash * std::pow(u, 2.0) + x_dash * u + y_dash;
                        const auto second_derivative = v_dash_dash * std::pow(u, 2.0) + w_dash_dash * u + x_dash_dash;

                        // Improve previous position
                        u -= first_derivative / second_derivative;
                    }

                    // Calculate other roots analytically
                    double u_2, u_3;
                    u_2 = u_3 = u;

                    const auto a_reduced = v_dash;
                    const auto b_reduced = w_dash + u * a_reduced;
                    const auto c_reduced = x_dash + u * b_reduced;

                    const auto discriminant = b_reduced * b_reduced - 4.0 * a_reduced * c_reduced;

                    if (fabsf(a_reduced) > delta&& discriminant > 0.0)
                    {
                        u_2 = (-b_reduced + sqrtf(discriminant)) / (2.0 * a_reduced);
                        u_3 = (-b_reduced - sqrtf(discriminant)) / (2.0 * a_reduced);
                    }

                    // "Clamp" u to the range [u_left, u_right]
                    if (u < u_left || u > u_right) u = u_left;
                    if (u_2 < u_left || u_2 > u_right) u_2 = u_left;
                    if (u_3 < u_left || u_3 > u_right) u_3 = u_left;

                    // Additionally get distances at the boundaries of the segment
                    const std::array<double, 5> us{
                        u,
                        u_2,
                        u_3,
                        u_left,
                        u_right
                    };

                    const std::array<double, 5> distances{
                        (point - (us[0] * us[0] * a + us[0] * b + c)).norm(),
                        (point - (us[1] * us[1] * a + us[1] * b + c)).norm(),
                        (point - (us[2] * us[2] * a + us[2] * b + c)).norm(),
                        (point - (us[3] * us[3] * a + us[3] * b + c)).norm(),
                        (point - (us[4] * us[4] * a + us[4] * b + c)).norm()
                    };

                    // Find smallest distance from the distances at the roots of the derivatives and the boundaries
                    // Set new smallest distance accordingly
                    distance_it->first = us[0];
                    distance_it->second = distances[0];

                    for (int i = 1; i < 5; ++i)
                    {
                        if (distances[i] < distance_it->second)
                        {
                            distance_it->first = us[i];
                            distance_it->second = distances[i];
                        }
                    }
                }
            }
            else if (degree > 2)
            {
                // Find the local minimum by linear subdivision
                for (auto distance_it = distances.begin(); distance_it != distances.end(); ++distance_it)
                {
                    // Get index and start at the center of the segment
                    const auto index = degree + static_cast<std::size_t>(std::floor((distance_it->first - u_begin) / delta));

                    auto u = distance_it->first;
                    auto u_left = u - 0.5 * delta;
                    auto u_right = u + 0.5 * delta;

                    // Loop a few times
                    bool good_match = false;

                    for (std::size_t i = 0; i < 10 && !good_match; ++i)
                    {
                        // The subdivision plane is defined by the position and the tangent at u
                        const auto position = compute_point(de_boor_points, knot_vector.cbegin(), knot_vector.cend(), this->Degree, u);
                        const auto tangent = compute_point(de_boor_points_first_derivative, knot_vector.cbegin() + 1, knot_vector.cend() - 1, this->Degree - 1, u);

                        const auto direction = tangent.dot(point - position);

                        if (std::abs(direction) < 0.0001)
                        {
                            good_match = true;
                        }
                        else if (direction > 0.0)
                        {
                            // In front of the plane
                            u_left = u;
                        }
                        else
                        {
                            // Behind the plane
                            u_right = u;
                        }

                        u = 0.5 * (u_left + u_right);
                    }

                    distance_it->first = u;
                    distance_it->second = (point - compute_point(de_boor_points, knot_vector.cbegin(), knot_vector.cend(), this->Degree, u)).norm();
                }
            }

            // Pick best of the results
            std::tie(nearest_arc, nearest_distance) = *std::min_element(distances.begin(), distances.end(), comparator);

            const auto position = compute_point(de_boor_points, knot_vector.cbegin(), knot_vector.cend(), this->Degree, nearest_arc);

            std::cout << "Resulting distances: " << print_collection(distances.begin(), distances.end()) << std::endl;

            // Output result
            std::cout << "Closest point at arc: " << nearest_arc << std::endl;
            std::cout << "Closest point position: [" << position[0] << ", " << position[1] << ", " << position[2] << "]" << std::endl;
            std::cout << "Distance: " << nearest_distance << std::endl;
        }
    }

    // Set output points
    auto output_points = vtkSmartPointer<vtkPoints>::New();
    output_points->SetNumberOfPoints(points.size());

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        output_points->SetPoint(i, points[i].data());
    }

    output->SetPoints(output_points);

    // Create array storing the arc position at each point
    auto output_arc_positions = vtkSmartPointer<vtkFloatArray>::New();
    output_arc_positions->SetNumberOfComponents(1);
    output_arc_positions->SetNumberOfTuples(points_arc_position.size());
    output_arc_positions->SetName("Parametric Position");

    for (std::size_t i = 0; i < points_arc_position.size(); ++i)
    {
        output_arc_positions->SetValue(i, points_arc_position[i]);
    }

    output->GetPointData()->AddArray(output_arc_positions);

    // Create arrays storing the orientation at each point
    if (degree > 2)
    {
        auto output_tangent = vtkSmartPointer<vtkFloatArray>::New();
        output_tangent->SetNumberOfComponents(3);
        output_tangent->SetNumberOfTuples(points_arc_position.size());
        output_tangent->SetName("Tangent");

        auto output_binormal = vtkSmartPointer<vtkFloatArray>::New();
        output_binormal->SetNumberOfComponents(3);
        output_binormal->SetNumberOfTuples(points_arc_position.size());
        output_binormal->SetName("Binormal");

        auto output_normal = vtkSmartPointer<vtkFloatArray>::New();
        output_normal->SetNumberOfComponents(3);
        output_normal->SetNumberOfTuples(points_arc_position.size());
        output_normal->SetName("Normal");

        for (std::size_t i = 0; i < points_arc_position.size(); ++i)
        {
            output_tangent->SetTuple(i, tangent[i].data());
            output_binormal->SetTuple(i, binormal[i].data());
            output_normal->SetTuple(i, normal[i].data());
        }

        output->GetPointData()->AddArray(output_tangent);
        output->GetPointData()->AddArray(output_binormal);
        output->GetPointData()->AddArray(output_normal);
    }

    // Set output line
    auto indices = vtkSmartPointer<vtkIdTypeArray>::New();
    indices->SetNumberOfComponents(1);
    indices->SetNumberOfTuples(points.size() + 1);

    indices->SetValue(0, points.size());

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        indices->SetValue(i + 1, i);
    }

    auto cell = vtkSmartPointer<vtkCellArray>::New();
    cell->SetCells(1, indices);

    output->SetLines(cell);

    return 1;
}

Eigen::Vector3d b_spline::compute_point(const std::vector<Eigen::Vector3d>& de_boor_points,
    std::vector<double>::const_iterator knot_vector, std::vector<double>::const_iterator knot_vector_end,
    const std::size_t degree, double arc_parameter) const
{
    Eigen::Vector3d point{ 0.0, 0.0, 0.0 };

    if (arc_parameter >= access_at(knot_vector, de_boor_points.size()))
    {
        arc_parameter = access_at(knot_vector, de_boor_points.size()) - 0.0001f;
    }

    for (std::size_t j = 0; j < de_boor_points.size(); ++j)
    {
        const auto N = basis_function(knot_vector, knot_vector_end, j, degree, arc_parameter);

        point[0] += N * de_boor_points[j][0];
        point[1] += N * de_boor_points[j][1];
        point[2] += N * de_boor_points[j][2];
    }

    return point;
}

double b_spline::basis_function(std::vector<double>::const_iterator knot_vector, std::vector<double>::const_iterator knot_vector_end,
    const std::size_t de_boor_index, const std::size_t degree, const double u) const
{
    // 1 if u_i <= u < u_i+1, 0 otherwise
    if (degree == 0)
    {
        return (access_at(knot_vector, de_boor_index) <= u && u < access_at(knot_vector, de_boor_index + 1)) ? 1.0 : 0.0;
    }

    // Calculate recursively
    const auto Ni = basis_function(knot_vector, knot_vector_end, de_boor_index, degree - 1, u);
    const auto Nip1 = basis_function(knot_vector, knot_vector_end, de_boor_index + 1, degree - 1, u);

    const auto part_1 = (access_at(knot_vector, de_boor_index + degree) - access_at(knot_vector, de_boor_index) == 0.0) ? 0.0 :
        ((u - access_at(knot_vector, de_boor_index)) / (access_at(knot_vector, de_boor_index + degree) - access_at(knot_vector, de_boor_index)));
    const auto part_2 = (access_at(knot_vector, de_boor_index + degree + 1) - access_at(knot_vector, de_boor_index + 1) == 0.0) ? 0.0 :
        ((access_at(knot_vector, de_boor_index + degree + 1) - u) / (access_at(knot_vector, de_boor_index + degree + 1) - access_at(knot_vector, de_boor_index + 1)));

    return part_1 * Ni + part_2 * Nip1;
}

std::vector<Eigen::Vector3d> b_spline::derive(const std::vector<Eigen::Vector3d>& de_boor_points,
    std::vector<double>::const_iterator knot_vector, std::vector<double>::const_iterator knot_vector_end, const std::size_t degree) const
{
    std::vector<Eigen::Vector3d> derived_de_boor_points(de_boor_points.size() - 1);

    for (std::size_t index = 0; index < derived_de_boor_points.size(); ++index)
    {
        derived_de_boor_points[index] = static_cast<double>(degree / (access_at(knot_vector, index + degree + 1) - access_at(knot_vector, index + 1)))
            * (de_boor_points[index + 1] - de_boor_points[index]);
    }

    return derived_de_boor_points;
}
