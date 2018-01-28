/*  This file is part of Spooky, a sensor fusion plugin for VR in the Unreal Engine

    Copyright 2017 Jake Fountain

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
#include "Spooky.h"
#include "ArticulatedModel.h"
#include "Utilities/CommonMath.h"

namespace spooky{

    //TODO: try this?
    // Node::getRequiredParentsCts(const Measurement::Ptr& m, const Transform3D& toFusionSpace){
    //         Eigen::Vector<float,6> error = toFusionSpace * m->getPosRot() - getGlobalPose().toPosRot();
            
    //         //Iterate through parents until enough flex is found
    //         std::vector<Node::Ptr> parents;
    //         parents.push_back(std::make_shared<Node>(this));
    //         Eigen::Matrix<float,6> varTotal = Eigen::Vector3f::Zero();
    //         Transform3D pose = Transform3D::Identity();
    //         float flex = 0;
    //         const float flex_threshold = 0.9;
    //         while(true){
    //             //Assume decoupling between nodes
    //             pose = parents.back()->getLocalPose() * pose;
    //             varTotal = parents.back()->getLocalPoseVariance(pose,varTotal);
    //             flex = std::exp(-error.transpose()*varTotal*error)
    //             if(flex<flex_threshold){
    //                 //We dont need anymore nodes to fuse data
    //                 break;
    //             } else {
    //                 //We need more nodes
    //                 parents.push_back(parents.back()->parent);
    //             }
    //         }
    //         return parents;
    // }

    int Node::getRequiredChainLength(const Measurement::Ptr& m){
        //Iterate through parents until enough flex is found
        int nodecount = 1;
        //Positional degrees of freedom
        int p_dof = 0;
        //rotational degrees of freedom
        int r_dof = 0;
        //Required dof:
        int p_dof_req = m->getRequiredPDoF();
        int r_dof_req = m->getRequiredRDoF();

        const float flex_threshold = 0.9;
        bool hasLeverChild = false;
		//TODO: do a better way?
		Node* node = this;
        while(true){
            //Assume decoupling between nodes
            p_dof += node->getPDoF(hasLeverChild);
            r_dof += node->getRDoF();

            if(!hasLeverChild){
                //If there is much of a translation then it creates a lever which can cause position change
                hasLeverChild = node->getLocalPose().translation().norm() > 0.01;
            }
            
            if(p_dof>=p_dof_req && r_dof>=r_dof_req || node->parent == NULL){
                //We dont need anymore nodes to fuse data, or we are out of nodes
                break;
            } else {
                //We need more nodes
				node = node->parent.get();
				nodecount++;
            }
        }
        return nodecount;
    }

    void Node::fusePositionMeasurement(const Measurement::Ptr& m, const Transform3D& toFusionSpace){
        //Calculate error
        if(m->globalSpace){
			//Fuse by modifying some parents if necessary
			int fusion_chain = getRequiredChainLength(m);
			//fuse
			Eigen::Matrix<float, 6, Eigen::Dynamic> measurementJacobian = getPoseChainJacobian(fusion_chain);
			State::Parameters chainState = getChainState(fusion_chain);
			//TODO: constraints
			State::Parameters constraints = getChainState(fusion_chain);

			State::Parameters newChainState = chainState.size();

			Eigen::AngleAxisf rot = Eigen::AngleAxisf(m->getRotation());
			Eigen::Matrix<float, 6, 1> wp;
			wp.head(3) = rot.angle() * rot.axis();
			wp.tail(3) = m->getPosition();
			Eigen::Matrix<float, 3, 4> quatToAxisJacobian = utility::getQuatToAxisJacobian(m->getRotation());

			//TODO: Fix quat to be consistent: x,y,z,w is how eigen stores it internally, but its consrtuctor uses Quat(w,x,y,z)
			Eigen::MatrixXf sigmaM_inv = (quatToAxisJacobian * m->getRotationVar() * quatToAxisJacobian.transpose()).inverse();
			Eigen::MatrixXf sigmaP_inv = chainState.variance.inverse();
			Eigen::MatrixXf sigmaC_inv = constraints.variance.inverse();
			
			float joint_stiffness = 0; //in [0,1]

			newChainState.variance = (measurementJacobian.transpose() * sigmaM_inv * measurementJacobian +
									  (1 / float(fusion_chain)) * (sigmaP_inv + joint_stiffness * sigmaC_inv)).inverse();
			newChainState.expectation = newChainState.variance * (measurementJacobian.transpose() * sigmaM_inv * wp +
																	(1 / float(fusion_chain)) *
																	(sigmaP_inv * chainState.expectation 
																		+ joint_stiffness * sigmaC_inv)
																	);

			setChainState(fusion_chain, newChainState);
        } else {
			//TODO: Fuse locally with reg kalman filter
			//insertMeasurement(m);
        }
        //TODO: fuse chain
    }

    void Node::fuseRotationMeasurement(const Measurement::Ptr& m, const Transform3D& toFusionSpace){

    }

    void Node::fuseRigidMeasurement(const Measurement::Ptr& m, const Transform3D& toFusionSpace){

    }

    void Node::fuseScaleMeasurement(const Measurement::Ptr& m, const Transform3D& toFusionSpace){

    }


	Eigen::Matrix<float, 6, Eigen::Dynamic> Node::getPoseChainJacobian(const int& chain_length) {
		//Precompute Jacobian size
		int inputDimension = 0;
		//TODO: dont use raw pointer
		Node* node = this;
		for (int i = 0; i < chain_length; i++) {
			inputDimension += node->getDimension();
			if (node->parent == NULL) break;
			node = node->parent.get();
		}
		//Reset for actual calculation
		node = this;
		float h = 1e-20;
		Transform3Dcd childPoses = Transform3Dcd::Identity();
		Transform3Dcd parentPoses = (node->parent == NULL) ? parent->getGlobalPose().cast<std::complex<double>>() : Transform3Dcd::Identity();
		Eigen::Matrix<float, 6, Eigen::Dynamic > J(6, inputDimension);
		int column = 0;
		for (int i = 0; i < chain_length; i++) {
			//Loop through all dof of this node and get the jacobian (w,p) entries for each dof
			int dof = node->getDimension();
			for (int j = 0; j < dof; j++) {
				//Complex step approximation
				J.col(column) = (utility::toAxisAnglePos((parentPoses * node->getLocalPoseComplexStep(j, h) * childPoses)) / std::complex<double>(h,0)).imag().cast<float>();
				column++;
			}
			//Move to next parent
			if (node->parent == NULL) break;
			childPoses = node->getLocalPose().cast<std::complex<double>>() * childPoses;
			node = node->parent.get();
			parentPoses = (node->parent == NULL) ? parent->getGlobalPose().cast<std::complex<double>>() : Transform3Dcd::Identity();
		}
		return J;
	}


	Transform3Dcd Node::getLocalPoseComplexStep(int j, double h) {
		Transform3Dcd pose = Transform3Dcd::Identity();
		int totalDim = 0;
		for (int i = 0; i < articulations.size(); i++) {
			int thisDim = local_state.articulation[i].expectation.size();
			int lastTotalDim = totalDim;
			totalDim += thisDim;
			Eigen::Matrix<std::complex<double>, Eigen::Dynamic,1> ih = Eigen::VectorXf::Zero(thisDim).cast<std::complex<double>>();
			if (j < totalDim && j >= lastTotalDim) {
				ih[j - lastTotalDim] = std::complex<double>(0,h);
			}
			pose = pose * articulations[i].getTransform<std::complex<double>>(local_state.articulation[i].expectation.cast<std::complex<double>>() + ih);
		}
		return pose;
	}


}