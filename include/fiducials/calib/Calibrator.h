#pragma once

#include <thread>
#include <mutex>
#include <memory>

#include <Sophus/se3.hpp>

#include <fiducials/cam/CameraModel.h>
#include <fiducials/calib/CostFunctionAndParams.h>
#include <fiducials/calib/AutoDiffArrayCostFunction.h>
#include <fiducials/calib/LocalParamSe3.h>

namespace fiducials {

template<typename T, typename ...Args>
std::unique_ptr<T> make_unique( Args&& ...args )
{
    return std::unique_ptr<T>( new T( std::forward<Args>(args)... ) );
}

template<typename ProjModel>
struct CameraAndPose
{
    CameraAndPose( const CameraModel<ProjModel>& camera, const Sophus::SE3d& T_ck)
        : camera(camera), T_ck(T_ck)
    {
    }

    CameraModel<ProjModel> camera;
    Sophus::SE3d T_ck;
};

// Parameter block 0: T_kw // keyframe
// Parameter block 1: T_ck // keyframe to cam
// Parameter block 2: fu,fv,u0,v0,w
template<typename ProjModel>
struct ReprojectionCost
    : public ceres::AutoDiffArrayCostFunction<
        CostFunctionAndParams, ReprojectionCost<ProjModel>,
        2,  7,7, ProjModel::NUM_PARAMS>
{
    ReprojectionCost(Eigen::Vector3d Pw, Eigen::Vector2d pc)
        : m_Pw(Pw), m_pc(pc)
    {        
    }
    
    template<typename T=double>
    bool Evaluate(T const* const* parameters, T* residuals) const
    {
        Eigen::Map<Eigen::Matrix<T,2,1> > r(residuals);
        const Eigen::Map<const Sophus::SE3Group<T> > T_kw(parameters[0]);
        const Eigen::Map<const Sophus::SE3Group<T> > T_ck(parameters[1]);
        T const* camparam = parameters[2];
        
        const Eigen::Matrix<T,3,1> Pc = T_ck * (T_kw * m_Pw.cast<T>());
        const Eigen::Matrix<T,2,1> pc = ProjModel::template Map<T>(Project<T>(Pc), camparam);
        r = pc - m_pc.cast<T>();
        return true;
    }    
    
    Eigen::Vector3d m_Pw;
    Eigen::Vector2d m_pc;
};

template<typename ProjModel>
class Calibrator
{
public:
    
    Calibrator()
    {
        m_prob_options.cost_function_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
        m_prob_options.local_parameterization_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
        m_prob_options.loss_function_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
    
        m_solver_options.num_threads = 4;
        m_solver_options.update_state_every_iteration = true;
        m_solver_options.max_num_iterations = 100;        
    }
    
    ~Calibrator()
    {
        std::cout << "------------------------------------------" << std::endl;
        for(size_t c=0; c<m_camera.size(); ++c) {
            std::cout << "Camera: " << c << std::endl;
            std::cout << m_camera[c]->camera.Params().transpose() << std::endl;
            std::cout << m_camera[c]->T_ck.matrix3x4() << std::endl << std::endl;
        }        
        
    }
    
    void Clear()
    {
        m_T_kw.clear();
        m_camera.clear();
        m_costs.clear();
    }
    
    void Start()
    {
        if(!m_running) {
            m_should_run = true;
            m_thread = std::thread(std::bind( &Calibrator::SolveThread, this )) ;
        }else{
            std::cerr << "Already Running." << std::endl;
        }        
    }
    
    void Stop()
    {
        m_should_run = false;
        m_thread.join();        
    }
    
    int AddCamera(const CameraModel<ProjModel>& cam, const Sophus::SE3d T_ck = Sophus::SE3d() )
    {
        int id = m_camera.size();
        m_camera.push_back( make_unique<CameraAndPose<ProjModel> >(cam,T_ck) );
        return id;
    }
    
    int AddFrame(Sophus::SE3d T_kw = Sophus::SE3d())
    {
        int id = m_T_kw.size();
        m_T_kw.push_back( make_unique<Sophus::SE3d>(T_kw) );
        return id;
    }
    
    void AddObservation(
        int frame, int camera,
        const Eigen::Vector3d& P_c,
        const Eigen::Vector2d& p_c
    ) {
        m_update_mutex.lock();

        // new camera pose to bundle adjust
        
        CameraAndPose<ProjModel>& cp = *m_camera[camera];
        Sophus::SE3d& T_kw = *m_T_kw[frame];
        
        // Create cost function
        CostFunctionAndParams* cost = new ReprojectionCost<ProjModel>(P_c, p_c);
        cost->Params() = std::vector<double*>{T_kw.data(), cp.T_ck.data(), cp.camera.data()};
        cost->Loss() = nullptr;
        m_costs.push_back(std::unique_ptr<CostFunctionAndParams>(cost));
        
        m_update_mutex.unlock();
    }
    
    size_t NumFrames() const
    {
        return m_T_kw.size();
    }
    
    Sophus::SE3d& GetFrame(size_t i)
    {
        return *m_T_kw[i];
    }
    
    size_t NumCameras() const
    {
        return m_camera.size();
    }
    
    CameraAndPose<ProjModel> GetCamera(size_t i)
    {
        return *m_camera[i];
    }
    
protected:
    
    void SolveThread()
    {
        m_running = true;
        while(m_should_run)
        {
            ceres::Problem problem(m_prob_options);
            
            m_update_mutex.lock();
            
            // Add parameters
            for(size_t c=0; c<m_camera.size(); ++c) {
                problem.AddParameterBlock(m_camera[c]->T_ck.data(), 7, &m_LocalParamSe3 );
                if(c==0) {
                    problem.SetParameterBlockConstant(m_camera[c]->T_ck.data());
                }
            }
            for(size_t p=0; p<m_T_kw.size(); ++p) {
                problem.AddParameterBlock(m_T_kw[p]->data(), 7, &m_LocalParamSe3 );
            }
            
            // Add costs
            for(size_t c=0; c<m_costs.size(); ++c)
            {
                CostFunctionAndParams& cost = *m_costs[c];
                problem.AddResidualBlock(&cost, cost.Loss(), cost.Params());
            }
            
            m_update_mutex.unlock();
            
            // Crank optimisation
            if(problem.NumResiduals() > 0) {
                try {
                    ceres::Solver::Summary summary;
                    ceres::Solve(m_solver_options, &problem, &summary);
                    std::cout << summary.BriefReport() << std::endl;     
                    const double mse = summary.final_cost / summary.num_residuals;
                    std::cout << "Frames: " << m_T_kw.size() << "; Observations: " << summary.num_residuals << "; mse: " << mse << std::endl;
                }catch(std::exception e) {
                    std::cerr << e.what() << std::endl;
                }
            }
        }
        m_running = false;
    }
    
    std::mutex m_update_mutex;    
    std::thread m_thread;    
    bool m_should_run;
    bool m_running;
    
    std::vector< std::unique_ptr<Sophus::SE3d> > m_T_kw;
    std::vector< std::unique_ptr<CameraAndPose<ProjModel> > > m_camera;    
    std::vector< std::unique_ptr<CostFunctionAndParams > > m_costs;  
    
    ceres::Problem::Options m_prob_options;
    ceres::Solver::Options  m_solver_options;
    LocalParamSe3  m_LocalParamSe3;    
};

}