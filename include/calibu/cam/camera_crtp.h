/*
   This file is part of the Calibu Project.
   https://github.com/gwu-robotics/Calibu

   Copyright (C) 2013 George Washington University,
                      Steven Lovegrove,
                      Nima Keivan
                      Gabe Sibley

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */
#pragma once
#include <Eigen/Eigen>
#include <sophus/se3.hpp>
namespace calibu
{
  template<typename Scalar> using Vec2t = Eigen::Matrix<Scalar, 2, 1>;
  template<typename Scalar> using Vec3t = Eigen::Matrix<Scalar, 3, 1>;
  template<typename Scalar> using VecXt =
    Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
  template<typename Scalar> using SE3t =
    Sophus::SE3Group<Scalar>;

  template<typename Scalar=double>
  class CameraInterface
  {
  public:
    typedef std::shared_ptr<CameraInterface> Ptr;
    virtual Vec3t<Scalar> Unproject(const Vec2t<Scalar>& pix) const = 0;
    virtual Vec2t<Scalar> Project(const Vec3t<Scalar>& ray) const = 0;
    virtual Eigen::Matrix<Scalar, 2, 3> dProject_dray(
        const Vec3t<Scalar>& ray) const = 0;
    virtual Vec2t<Scalar> Transfer3d(const SE3t<Scalar>& t_ba,
                                     const Vec3t<Scalar>& ray,
                                     const Scalar rho) const = 0;
    virtual Eigen::Matrix<Scalar, 2, 4> dTransfer3d_dray(
        const SE3t<Scalar>& t_ba,
        const Vec3t<Scalar>& ray,
        const Scalar rho) const = 0;
  };

  // The Curiously Recurring Template Pattern (CRTP)
  template<class Derived, typename Scalar = double>
  class Camera : public CameraInterface<Scalar>
  {
  public:
    virtual ~Camera() {}

    Vec3t<Scalar> Unproject(const Vec2t<Scalar>& pix) const
    {
      Vec3t<Scalar> ray;
      static_cast<const Derived*>(this)->UnprojectImpl(pix.data(), ray.data());
      return ray;
    }

    Vec2t<Scalar> Project(const Vec3t<Scalar>& ray) const
    {
      Vec2t<Scalar> pix;
      static_cast<const Derived*>(this)->ProjectImpl(ray.data(), pix.data());
      return pix;
    }

    Eigen::Matrix<Scalar, 2, 3> dProject_dray(
        const Vec3t<Scalar>& ray) const
    {
      Eigen::Matrix<Scalar, 2, 3> j;
      static_cast<const Derived*>(this)->dProject_drayImpl(
            ray.data(), j.data());
      return j;
    }

    Vec2t<Scalar> Transfer3d(const SE3t<Scalar>& t_ba,
                             const Vec3t<Scalar>& ray,
                             const Scalar rho) const
    {
      const Vec3t<Scalar> ray_dehomogenized =
          t_ba.rotationMatrix() * ray + rho * t_ba.translation();
      return Project(ray_dehomogenized);
    }

    Eigen::Matrix<Scalar, 2, 4> dTransfer3d_dray(const SE3t<Scalar>& t_ba,
                                                 const Vec3t<Scalar>& ray,
                                                 const Scalar rho) const
    {
      const Eigen::Matrix<Scalar, 3, 3> rot_matrix = t_ba.rotationMatrix();
      const Vec3t<Scalar> ray_dehomogenized =
          rot_matrix * ray + rho * t_ba.translation();
      const Eigen::Matrix<Scalar, 2, 3> dproject_dray =
          dProject_dray(ray_dehomogenized);
      Eigen::Matrix<Scalar, 2, 4> dtransfer3d_dray;
      dtransfer3d_dray.template topLeftCorner<2, 3>() =
          dproject_dray * rot_matrix;
      dtransfer3d_dray.col(3) = dproject_dray * t_ba.translation();
      return dtransfer3d_dray;
    }

  protected:
    Camera()
    {
    }
  };

  template<typename Scalar=double>
  class Rig
  {
  public:
    void AddCamera(typename CameraInterface<Scalar>::Ptr cam,
                   const SE3t<Scalar>& t_wc)
    {
      cameras_.push_back(cam);
      t_wc_.push_back(t_wc);
    }

    void AddCamera(CameraInterface<Scalar>* cam,
                   const SE3t<Scalar>& t_wc)
    {
      cameras_.push_back(typename CameraInterface<Scalar>::Ptr(cam));
      t_wc_.push_back(t_wc);
    }

    std::vector<typename CameraInterface<Scalar>::Ptr> cameras_;
    std::vector<SE3t<Scalar>> t_wc_;
  };


}