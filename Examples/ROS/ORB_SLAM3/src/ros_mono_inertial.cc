/**
* This file is part of ORB-SLAM3
*
* Copyright (C) 2017-2020 Carlos Campos, Richard Elvira, Juan J. Gómez Rodríguez, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
* Copyright (C) 2014-2016 Raúl Mur-Artal, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
*
* ORB-SLAM3 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
* License as published by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM3 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
* the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with ORB-SLAM3.
* If not, see <http://www.gnu.org/licenses/>.
*/

#include<iostream>
#include<algorithm>
#include<fstream>
#include<chrono>
#include<vector>
#include<queue>
#include<thread>
#include<mutex>

#include<ros/ros.h>
#include<cv_bridge/cv_bridge.h>
#include<sensor_msgs/Imu.h>

#include<opencv2/core/core.hpp>

#include"../../../include/System.h"
#include"../include/ImuTypes.h"
#include "ORB_SLAM3/RigidBody.h"
#include "ORB_SLAM3/RigidBody.h"
#include "ORB_SLAM3/Marker.h"
#include "ORB_SLAM3/TrackData.h"

using namespace std;

class ImuGrabber
{
public:
	ImuGrabber()
	{
	};

	void GrabImu(const sensor_msgs::ImuConstPtr& imu_msg);

	queue<sensor_msgs::ImuConstPtr> imuBuf;
	std::mutex mBufMutex;
};

class ImageGrabber
{
public:
	ImageGrabber(ORB_SLAM3::System* pSLAM, ImuGrabber* pImuGb, const bool bClahe) : mpSLAM(pSLAM), mpImuGb(pImuGb),
																					mbClahe(bClahe)
	{
	}

	void GrabImage(const sensor_msgs::ImageConstPtr& msg);

	cv::Mat GetImage(const sensor_msgs::ImageConstPtr& img_msg);

	void SyncWithImu();

	queue<sensor_msgs::ImageConstPtr> img0Buf;
	std::mutex mBufMutex;

	ORB_SLAM3::System* mpSLAM;
	ImuGrabber* mpImuGb;

	const bool mbClahe;
	cv::Ptr<cv::CLAHE> mClahe = cv::createCLAHE(3.0, cv::Size(8, 8));
};

void GetOptiTrackCallback(const ORB_SLAM3::TrackData::ConstPtr& msg)
{
	int bodyCnt = msg->rigid_body_quantity;
	if(bodyCnt != 0)
	{
		ORB_SLAM3::System::mOptiTrackMutex.lock();
		ORB_SLAM3::System::optiTrackRigidBodies.clear();
		ORB_SLAM3::System::mOptiTrackMutex.unlock();

		for (int i = 0; i < msg->rigid_body_quantity; ++i)
		{
			RigidBody rigidBody;
			rigidBody.id = msg->rigid_bodies[i].id;
			rigidBody.pose.position.x = msg->rigid_bodies[i].pose.position.x;
			rigidBody.pose.position.y = msg->rigid_bodies[i].pose.position.y;
			rigidBody.pose.position.z = msg->rigid_bodies[i].pose.position.z;
			rigidBody.pose.orientation.x = msg->rigid_bodies[i].pose.orientation.x;
			rigidBody.pose.orientation.y = msg->rigid_bodies[i].pose.orientation.y;
			rigidBody.pose.orientation.z = msg->rigid_bodies[i].pose.orientation.z;
			rigidBody.pose.orientation.w = msg->rigid_bodies[i].pose.orientation.w;
			rigidBody.mean_error = msg->rigid_bodies[i].mean_error;
			rigidBody.tracking_flag = msg->rigid_bodies[i].tracking_flag;
			ORB_SLAM3::System::mOptiTrackMutex.lock();
			ORB_SLAM3::System::optiTrackRigidBodies.push_back(rigidBody);
			ORB_SLAM3::System::mOptiTrackMutex.unlock();
		}

		ORB_SLAM3::System::mOptiTrackMutex.lock();
		for (int i = 0; i < ORB_SLAM3::System::optiTrackRigidBodies.size(); ++i)
		{
			ROS_INFO("ORB_SLAM3::System::optiTrackRigidBodies position [x y]: [%f %f]", ORB_SLAM3::System::optiTrackRigidBodies[i].pose.position.x, ORB_SLAM3::System::optiTrackRigidBodies[i].pose.position.y);
		}
		ORB_SLAM3::System::mOptiTrackMutex.unlock();

		ORB_SLAM3::System::mOptiTrackQueueMutex.lock();
		if(!ORB_SLAM3::System::optiTrackRigidBodiesQueue.empty() && ORB_SLAM3::System::optiTrackRigidBodiesQueue.size() > 100)
		{
			ORB_SLAM3::System::optiTrackRigidBodiesQueue.clear();
		}
		ORB_SLAM3::System::mOptiTrackMutex.lock();
		ORB_SLAM3::System::optiTrackRigidBodiesQueue.push_back(ORB_SLAM3::System::optiTrackRigidBodies);
		ORB_SLAM3::System::mOptiTrackMutex.unlock();
		ORB_SLAM3::System::mOptiTrackQueueMutex.unlock();
	}
	else
	{
		ROS_ERROR("No rigidBody in Opti Track data, check please.");
	}
}

int main(int argc, char** argv)
{
	ros::init(argc, argv, "Mono_Inertial");
	ros::NodeHandle n("~");
	ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
	bool bEqual = false;
	if (argc < 3 || argc > 4)
	{
		cerr << endl << "Usage: rosrun ORB_SLAM3 Mono_Inertial path_to_vocabulary path_to_settings [do_equalize]"
			 << endl;
		ros::shutdown();
		return 1;
	}

	if (argc == 4)
	{
		std::string sbEqual(argv[3]);
		if (sbEqual == "true")
			bEqual = true;
	}

	// Create SLAM system. It initializes all system threads and gets ready to process frames.
	ORB_SLAM3::System SLAM(argv[1], argv[2], ORB_SLAM3::System::IMU_MONOCULAR, true);

	ImuGrabber imugb;
	ImageGrabber igb(&SLAM, &imugb, bEqual); // TODO

	// Maximum delay, 5 seconds
	ros::Subscriber sub_imu = n.subscribe("/camera/imu", 1000, &ImuGrabber::GrabImu, &imugb);
	ros::Subscriber sub_img0 = n.subscribe("/camera/image_raw", 100, &ImageGrabber::GrabImage, &igb);
	ros::Subscriber sub_optitrack = n.subscribe("/opti_track_node/opti_track_data", 500, GetOptiTrackCallback);

	std::thread sync_thread(&ImageGrabber::SyncWithImu, &igb);

	ros::spin();

	SLAM.Shutdown();
	SLAM.SaveTrajectoryEuRoCAndOptiTrack("/midea_robot/ros1_ws/orbslam.txt", "/midea_robot/ros1_ws/optitrack.txt");

	return 0;
}

void ImageGrabber::GrabImage(const sensor_msgs::ImageConstPtr& img_msg)
{
	mBufMutex.lock();
	if (!img0Buf.empty())
		img0Buf.pop();
	img0Buf.push(img_msg);
	mBufMutex.unlock();
}

cv::Mat ImageGrabber::GetImage(const sensor_msgs::ImageConstPtr& img_msg)
{
	// Copy the ros image message to cv::Mat.
	cv_bridge::CvImageConstPtr cv_ptr;
	try
	{
		cv_ptr = cv_bridge::toCvShare(img_msg, sensor_msgs::image_encodings::MONO8);
	}
	catch (cv_bridge::Exception& e)
	{
		ROS_ERROR("cv_bridge exception: %s", e.what());
	}

	if (cv_ptr->image.type() == 0)
	{
		return cv_ptr->image.clone();
	}
	else
	{
		std::cout << "Error type" << std::endl;
		return cv_ptr->image.clone();
	}
}

void ImageGrabber::SyncWithImu()
{
	while (1)
	{
		cv::Mat im;
		double tIm = 0;
		if (!img0Buf.empty() && !mpImuGb->imuBuf.empty())
		{
			tIm = img0Buf.front()->header.stamp.toSec();
			if (tIm > mpImuGb->imuBuf.back()->header.stamp.toSec())
				continue;
			{
				this->mBufMutex.lock();
				im = GetImage(img0Buf.front());
				img0Buf.pop();
				this->mBufMutex.unlock();
			}

			vector<ORB_SLAM3::IMU::Point> vImuMeas;
			mpImuGb->mBufMutex.lock();
			if (!mpImuGb->imuBuf.empty())
			{
				// Load imu measurements from buffer
				vImuMeas.clear();
				while (!mpImuGb->imuBuf.empty() && mpImuGb->imuBuf.front()->header.stamp.toSec() <= tIm)
				{
					double t = mpImuGb->imuBuf.front()->header.stamp.toSec();
					cv::Point3f acc(mpImuGb->imuBuf.front()->linear_acceleration.x,
							mpImuGb->imuBuf.front()->linear_acceleration.y,
							mpImuGb->imuBuf.front()->linear_acceleration.z);
					cv::Point3f gyr(mpImuGb->imuBuf.front()->angular_velocity.x,
							mpImuGb->imuBuf.front()->angular_velocity.y, mpImuGb->imuBuf.front()->angular_velocity.z);
					vImuMeas.push_back(ORB_SLAM3::IMU::Point(acc, gyr, t));
					mpImuGb->imuBuf.pop();
				}
			}
			mpImuGb->mBufMutex.unlock();
			if (mbClahe)
				mClahe->apply(im, im);

			mpSLAM->TrackMonocular(im, tIm, vImuMeas);
		}

		std::chrono::milliseconds tSleep(1);
		std::this_thread::sleep_for(tSleep);
	}
}

void ImuGrabber::GrabImu(const sensor_msgs::ImuConstPtr& imu_msg)
{
	mBufMutex.lock();
	imuBuf.push(imu_msg);
	mBufMutex.unlock();
	return;
}


