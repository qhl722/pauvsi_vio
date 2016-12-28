/*
 * vio.cpp
 *
 *  Created on: Sep 19, 2016
 *      Author: kevinsheridan
 */

#include "vio.h"


/*
 * starts all state vectors at 0.0
 */
VIO::VIO()
{
	this->readROSParameters();

	//tf2_ros::TransformListener tf_listener(tfBuffer); // starts a thread which keeps track of transforms in the system

	//feature tracker pass it its params
	this->feature_tracker.setParams(FEATURE_SIMILARITY_THRESHOLD, MIN_EIGEN_VALUE,
			KILL_BY_DISSIMILARITY, NUM_FEATURES, MIN_EIGEN_VALUE);

	//set up image transport
	image_transport::ImageTransport it(nh);
	this->cameraSub = it.subscribeCamera(this->getCameraTopic(), 1, &VIO::cameraCallback, this);

	//setup imu sub
	this->imuSub = nh.subscribe(this->getIMUTopic(), 100, &VIO::imuCallback, this);

	ekf.setGravityMagnitude(this->GRAVITY_MAG); // set the gravity mag

	this->broadcastWorldToOdomTF();

	//setup pointcloudPublisher
	if(PUBLISH_ACTIVE_FEATURES)
		activePointsPub = nh.advertise<sensor_msgs::PointCloud>("/vio/activefeatures", 100);

	started = false; //not intialized yet
}

VIO::~VIO()
{

}

void VIO::cameraCallback(const sensor_msgs::ImageConstPtr& img, const sensor_msgs::CameraInfoConstPtr& cam)
{
	ros::Time start = ros::Time::now();
	cv::Mat temp = cv_bridge::toCvShare(img, "mono8")->image.clone();

	//set the K and D matrices
	this->setK(get3x3FromVector(cam->K));
	this->setD(cv::Mat(cam->D, false));

	//undistort the image using the fisheye model
	//ROS_ASSERT(cam->distortion_model == "fisheye");
	//cv::fisheye::undistortImage(temp, temp, this->K, this->D, this->K);

	// set the current frame
	this->setCurrentFrame(temp, cv_bridge::toCvCopy(img, "mono8")->header.stamp);

	//set the current frame's K & D
	this->currentFrame.K = this->K;
	this->currentFrame.D = this->D;

	// process the frame correspondences
	this->run();

	//get the run time
	ROS_DEBUG_STREAM_THROTTLE(0.5, (ros::Time::now().toSec() - start.toSec()) * 1000 << " milliseconds runtime");

	this->viewImage(this->getCurrentFrame());
}

void VIO::imuCallback(const sensor_msgs::ImuConstPtr& msg)
{
	//ROS_DEBUG_STREAM_THROTTLE(0.1, "accel: " << msg->linear_acceleration);
	this->ekf.addIMUMessage(*msg);
	//ROS_DEBUG_STREAM("time compare " << ros::Time::now().toNSec() - msg->header.stamp.toNSec());
}

cv::Mat VIO::get3x3FromVector(boost::array<double, 9> vec)
{
	cv::Mat mat = cv::Mat(3, 3, CV_32F);
	for(int i = 0; i < 3; i++)
	{
		mat.at<float>(i, 0) = vec.at(3 * i + 0);
		mat.at<float>(i, 1) = vec.at(3 * i + 1);
		mat.at<float>(i, 2) = vec.at(3 * i + 2);
	}

	ROS_DEBUG_STREAM_ONCE("K = " << mat);
	return mat;
}

/*
 * shows cv::Mat
 */
void VIO::viewImage(cv::Mat img){
	cv::imshow("test", img);
	cv::waitKey(30);
}

/*
 * draws frame with its features
 */
void VIO::viewImage(Frame frame){
	cv::Mat img;
	cv::drawKeypoints(frame.image, frame.getKeyPointVectorFromFeatures(), img, cv::Scalar(0, 0, 255));
	cv::drawKeypoints(img, frame.getUndistortedKeyPointVectorFromFeatures(), img, cv::Scalar(255, 0, 0));
	this->viewImage(img);

}

/*
 * sets the current frame and computes important
 * info about it
 * finds corners
 * describes corners
 */
void VIO::setCurrentFrame(cv::Mat img, ros::Time t)
{
	if(currentFrame.isFrameSet())
	{
		//first set the last frame to current frame
		lastFrame = currentFrame;
	}

	currentFrame = Frame(img, t, lastFrame.nextFeatureID); // create a frame with a starting ID of the last frame's next id
}

/*
 * runs:
 * feature detection, ranking, flowing
 * motion estimation
 * feature mapping
 */
void VIO::run()
{
	// if there is a last frame, flow features and estimate motion
	if(lastFrame.isFrameSet())
	{
		if(lastFrame.features.size() > 0)
		{
			feature_tracker.flowFeaturesToNewFrame(lastFrame, currentFrame);
			currentFrame.cleanUpFeaturesByKillRadius(this->KILL_RADIUS);
			//this->checkFeatureConsistency(currentFrame, this->FEATURE_SIMILARITY_THRESHOLD);

			currentFrame.undistortFeatures(); // undistort the new features
		}

		//MOTION ESTIMATION
		this->lastState = this->state;
		this->state = this->estimateMotion(this->lastState, this->lastFrame, this->currentFrame);

		if(this->started) // if initialized
		{
			//UPDATE 3D ACTIVE AND INACTIVE FEATURES
			this->update3DFeatures(this->state, this->lastState, this->currentFrame, this->lastFrame);
		}
	}

	//check the number of 2d features in the current frame
	//if this is below the required amount refill the feature vector with
	//the best new feature. It must not be redundant.

	//ROS_DEBUG_STREAM("feature count: " << currentFrame.features.size());

	if(currentFrame.features.size() < this->NUM_FEATURES)
	{
		//add n new unique features
		//ROS_DEBUG("low on features getting more");
		currentFrame.getAndAddNewFeatures(this->NUM_FEATURES - currentFrame.features.size(), this->FAST_THRESHOLD, this->KILL_RADIUS, this->MIN_NEW_FEATURE_DISTANCE);
		//currentFrame.describeFeaturesWithBRIEF();

		currentFrame.undistortFeatures(); // undistort the new features
	}

	this->broadcastWorldToOdomTF();
	this->publishActivePoints();

	//ROS_DEBUG_STREAM("imu readings: " << this->imuMessageBuffer.size());
}

/*
 * publishes all active points in the list using the publisher if the user has specified
 */
void VIO::publishActivePoints()
{
	if(this->PUBLISH_ACTIVE_FEATURES)
	{

		sensor_msgs::PointCloud pc;

		std::vector<geometry_msgs::Point32> point;
		std::vector<sensor_msgs::ChannelFloat32> colors;

		pc.header.frame_id = this->world_frame;

		for(int i = 0; i < this->active3DFeatures.size(); i++)
		{
			//debugFeature(this->active3DFeatures.at(i));

			std::vector<float> intensity;
			sensor_msgs::ChannelFloat32 c;

			intensity.push_back(this->active3DFeatures.at(i).color[0]);
			//intensity.push_back(this->active3DFeatures.at(i).color[1]);
			//intensity.push_back(this->active3DFeatures.at(i).color[2]);

			c.values = intensity;
			c.name = "intensity";

			geometry_msgs::Point32 pt;
			pt.x = this->active3DFeatures.at(i).position[0];
			pt.y = this->active3DFeatures.at(i).position[1];
			pt.z = this->active3DFeatures.at(i).position[2];

			point.push_back(pt);
			colors.push_back(c);

		}

		pc.points = point;
		pc.channels = colors;

		this->activePointsPub.publish(pc); // publish!
	}
}

void VIO::debugFeature(VIOFeature3D f)
{
	ROS_DEBUG_STREAM("3D FEATURE:\nX: " << f.position[0] <<"\nY: " << f.position[1] << "\nZ: " <<
			f.position[2] << "\nLink - ID: " << f.current2DFeatureMatchID << " Index: " << f.current2DFeatureMatchIndex <<
			"\ncov: " << f.variance << "\ncolor" << f.color.val[0]);
}

/**
 From "Triangulation", Hartley, R.I. and Sturm, P., Computer vision and image understanding, 1997
 */
cv::Mat_<double> VIO::LinearLSTriangulation(cv::Point3d u,       //homogenous image point (u,v,1)
		cv::Matx34d P,       //camera 1 matrix
		cv::Point3d u1,      //homogenous image point in 2nd camera
		cv::Matx34d P1       //camera 2 matrix
)
{
	//build matrix A for homogenous equation system Ax = 0
	//assume X = (x,y,z,1), for Linear-LS method
	//which turns it into a AX = B system, where A is 4x3, X is 3x1 and B is 4x1
	cv::Matx43d A(u.x*P(2,0)-P(0,0),    u.x*P(2,1)-P(0,1),      u.x*P(2,2)-P(0,2),
			u.y*P(2,0)-P(1,0),    u.y*P(2,1)-P(1,1),      u.y*P(2,2)-P(1,2),
			u1.x*P1(2,0)-P1(0,0), u1.x*P1(2,1)-P1(0,1),   u1.x*P1(2,2)-P1(0,2),
			u1.y*P1(2,0)-P1(1,0), u1.y*P1(2,1)-P1(1,1),   u1.y*P1(2,2)-P1(1,2)
	);
	cv::Mat_<double> B = (cv::Mat_<double>(4,1) <<    -(u.x*P(2,3)    -P(0,3)),
			-(u.y*P(2,3)  -P(1,3)),
			-(u1.x*P1(2,3)    -P1(0,3)),
			-(u1.y*P1(2,3)    -P1(1,3)));

	cv::Mat_<double> X;
	cv::solve(A,B,X, cv::DECOMP_SVD);

	return X;
}

/*
 * this function uses the previous state and the current state
 * along with the previous frame and current frame to
 * update each 3d feature and add new 3d features if necessary
 * If 3d feature is not updated it will be either removed or added to the inactive list.
 */
void VIO::update3DFeatures(VIOState x, VIOState x_last, Frame cf, Frame lf)
{
	std::vector<VIOFeature3D> inactives = this->active3DFeatures; // set the new inactive features to be the current active features
	std::vector<VIOFeature3D> actives;

	tf::StampedTransform base2cam;
	try{
		this->ekf.tf_listener.lookupTransform(this->camera_frame, this->CoM_frame, ros::Time(0), base2cam);
	}
	catch(tf::TransformException e){
		ROS_WARN_STREAM(e.what());
	}

	//compute the rt matrices
	cv::Matx34d A = this->lastState.getRTMatrix(base2cam);
	cv::Matx34d B = this->state.getRTMatrix(base2cam);

	for(int i = 0; i < cf.features.size(); i++)
	{
		if(cf.features.at(i).isMatched()) // if this feature is matched
		{
			// store the currentFeature and lastFeature
			VIOFeature2D current2DFeature = cf.features.at(i);
			VIOFeature2D last2DFeature = lf.features.at(current2DFeature.getMatchedIndex()); // its matched 2d feature from the last frame

			ROS_ASSERT(current2DFeature.getMatchedID() == last2DFeature.getFeatureID()); // ensure that this is the right feature

			//check if this feature has a matched 3d feature
			bool match3D = false;
			VIOFeature3D matched3DFeature;
			for(int j = 0; j < inactives.size(); j++)
			{
				if(inactives.at(j).current2DFeatureMatchIndex == last2DFeature.getMatchedIndex())
				{
					matched3DFeature = inactives.at(j); // found a matched feature

					ROS_ASSERT(matched3DFeature.current2DFeatureMatchID == last2DFeature.getMatchedID()); // ensure that everything matches up

					match3D = true; //set the flag for later

					inactives.erase(inactives.begin() + j); // erase the jth feature from inactives

					break;
				}
			}

			//IMPORTANT TUNING EQUATION!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! TODO
			double dx = current2DFeature.getUndistorted().x - last2DFeature.getUndistorted().x;
			double dy = current2DFeature.getUndistorted().y - last2DFeature.getUndistorted().y;
			double d = sqrt(dx * dx + dy * dy);
			if(d == 0)
				d = 0.0001;

			//double r_cov_avg = (state.covariance(0, 0) + state.covariance(1, 1) + state.covariance(2, 2) + lastState.covariance(0, 0) + lastState.covariance(1, 1) + lastState.covariance(2, 2)) / 6.0;

			double cov = (1 / d) * 100000; // this is a very simple way of determining how certain we are of this point's 3d pos

			//ROS_DEBUG_STREAM("feature delta: " << d << " feature cov: " << cov);


			//triangulate point

			cv::Point3d pt1, pt2;
			pt1.x = last2DFeature.getUndistorted().x;
			pt1.y = last2DFeature.getUndistorted().y;
			pt1.z = 1;

			pt2.x = current2DFeature.getUndistorted().x;
			pt2.y = current2DFeature.getUndistorted().y;
			pt2.z = 1;

			//ROS_DEBUG_STREAM_ONCE("K Type: " << this->lastFrame.K.type() << " RT Type: " << A.type());
			ROS_DEBUG_STREAM("RT1 - " << A);
			ROS_DEBUG_STREAM("RT2 - " << B);
			ROS_DEBUG_STREAM("pt1 - " << pt1);
			ROS_DEBUG_STREAM("pt2 - " << pt2);

			cv::Mat_<double> X = this->LinearLSTriangulation(pt1, A, pt2, B);

			ROS_DEBUG_STREAM("homo point: " << X);

			Eigen::Vector3d r = Eigen::Vector3d(X(0) / X(3), X(1) / X(3), X(2) / X(3));


			if(match3D) // if this 2d feature has a matching 3d feature
			{
				//ROS_DEBUG_STREAM("matched 3d point");

				//ROS_DEBUG_STREAM("point pos: " << r);

				//update the feature
				matched3DFeature.update(r, cov);

				matched3DFeature.current2DFeatureMatchID = current2DFeature.getMatchedID();
				matched3DFeature.current2DFeatureMatchIndex = current2DFeature.getMatchedIndex();

				// append feature to actives
				actives.push_back(matched3DFeature);

			}
			else // if this 2d feature does'nt have a matching 3d feature
			{
				//ROS_DEBUG_STREAM("new 3d point");

				//ROS_DEBUG_STREAM("point pos: " << r);

				cv::Scalar color = this->currentFrame.image.at<cv::Scalar>(current2DFeature.getFeature().pt);

				//ROS_DEBUG_STREAM("intensity: " << color[0]);

				VIOFeature3D newFeat = VIOFeature3D(current2DFeature.getMatchedIndex(), current2DFeature.getMatchedID(), color, cov, r);

				actives.push_back(newFeat);

			}
		}
	}

	//set each of the 3d feature buffers to be published
	this->active3DFeatures = actives;
	this->inactive3DFeatures = inactives;
}

/*
 * gets parameters from ROS param server
 */
void VIO::readROSParameters()
{
	//CAMERA TOPIC
	ROS_WARN_COND(!ros::param::has("~cameraTopic"), "Parameter for 'cameraTopic' has not been set");
	ros::param::param<std::string>("~cameraTopic", cameraTopic, DEFAULT_CAMERA_TOPIC);
	ROS_DEBUG_STREAM("camera topic is: " << cameraTopic);

	//IMU TOPIC
	ROS_WARN_COND(!ros::param::has("~imuTopic"), "Parameter for 'imuTopic' has not been set");
	ros::param::param<std::string>("~imuTopic", imuTopic, DEFAULT_IMU_TOPIC);
	ROS_DEBUG_STREAM("IMU topic is: " << imuTopic);

	ros::param::param<std::string>("~imu_frame_name", imu_frame, DEFAULT_IMU_FRAME_NAME);
	ros::param::param<std::string>("~camera_frame_name", camera_frame, DEFAULT_CAMERA_FRAME_NAME);
	ros::param::param<std::string>("~odom_frame_name", odom_frame, DEFAULT_ODOM_FRAME_NAME);
	ros::param::param<std::string>("~center_of_mass_frame_name", CoM_frame, DEFAULT_COM_FRAME_NAME);
	ros::param::param<std::string>("~world_frame_name", world_frame, DEFAULT_WORLD_FRAME_NAME);
	ekf.imu_frame = imu_frame;
	ekf.camera_frame = camera_frame;
	ekf.odom_frame = odom_frame;
	ekf.CoM_frame = CoM_frame;
	ekf.world_frame = world_frame;

	ros::param::param<int>("~fast_threshold", FAST_THRESHOLD, DEFAULT_FAST_THRESHOLD);

	ros::param::param<float>("~feature_kill_radius", KILL_RADIUS, DEFAULT_2D_KILL_RADIUS);

	ros::param::param<int>("~feature_similarity_threshold", FEATURE_SIMILARITY_THRESHOLD, DEFAULT_FEATURE_SIMILARITY_THRESHOLD);
	ros::param::param<bool>("~kill_by_dissimilarity", KILL_BY_DISSIMILARITY, false);

	ros::param::param<float>("~min_eigen_value", MIN_EIGEN_VALUE, DEFAULT_MIN_EIGEN_VALUE);

	ros::param::param<int>("~num_features", NUM_FEATURES, DEFAULT_NUM_FEATURES);

	ros::param::param<int>("~min_new_feature_distance", MIN_NEW_FEATURE_DISTANCE, DEFAULT_MIN_NEW_FEATURE_DIST);

	ros::param::param<double>("~starting_gravity_mag", GRAVITY_MAG, DEFAULT_GRAVITY_MAGNITUDE);

	ros::param::param<double>("~recalibration_threshold", RECALIBRATION_THRESHOLD, DEFAULT_RECALIBRATION_THRESHOLD);

	ros::param::param<bool>("~publish_active_features", PUBLISH_ACTIVE_FEATURES, DEFAULT_PUBLISH_ACTIVE_FEATURES);

	ros::param::param<std::string>("~active_features_topic", ACTIVE_FEATURES_TOPIC, DEFAULT_ACTIVE_FEATURES_TOPIC);

	ros::param::param<double>("~min_triangualtion_dist", MIN_TRIANGUALTION_DIST, DEFAULT_MIN_TRIANGUALTION_DIST);

	ros::param::param<double>("~min_start_dist", MIN_START_DIST, DEFAULT_MIN_START_DIST);
}

/*
 * broadcasts the world to odom transform
 */
void VIO::broadcastWorldToOdomTF()
{
	//ROS_DEBUG_STREAM("state " << this->state.vector);
	static tf::TransformBroadcaster br;
	tf::Transform transform;
	transform.setOrigin(tf::Vector3(state.x(), state.y(), state.z()));

	//ROS_DEBUG_STREAM(this->pose.pose.orientation.w << " " << this->pose.pose.orientation.x);
	transform.setRotation(state.getTFQuaternion());
	br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), this->world_frame, this->odom_frame));
}

/*
 * broadcasts the odom to tempIMU trans
 */
ros::Time VIO::broadcastOdomToTempIMUTF(double roll, double pitch, double yaw, double x, double y, double z)
{
	static tf::TransformBroadcaster br;
	tf::Transform transform;
	transform.setOrigin(tf::Vector3(x, y, z));
	tf::Quaternion q;
	q.setRPY(roll, pitch, yaw);
	//ROS_DEBUG_STREAM(q.getW() << ", " << q.getX() << ", " << q.getY() << ", " << q.getZ());
	transform.setRotation(q);
	ros::Time sendTime = ros::Time::now();
	br.sendTransform(tf::StampedTransform(transform, sendTime, this->camera_frame, "temp_imu_frame"));
	return sendTime;
}

void VIO::correctOrientation(tf::Quaternion q, double certainty)
{
	//check if quats are nan
	ROS_ASSERT(state.q0() == state.q0());
	ROS_ASSERT(q.getW() == q.getW());
	//Takes orientation and rotates it towards q.
	state.setQuaternion(state.getTFQuaternion().slerp(q, certainty));
}
/*
 * recalibrates the state using average pixel motion
 * uses an Extended Kalman Filter to predict and update the state and its
 * covariance.
 */
VIOState VIO::estimateMotion(VIOState x, Frame lastFrame, Frame currentFrame)
{
	// recalibrate
	static bool consecutiveRecalibration = false;
	double avgFeatureChange = feature_tracker.averageFeatureChange(lastFrame, currentFrame); // get the feature change between f1 and f2

	//recalibrate the state using avg pixel change and track consecutive runs
	if(avgFeatureChange <= this->RECALIBRATION_THRESHOLD)
	{
		this->recalibrateState(avgFeatureChange, this->RECALIBRATION_THRESHOLD, consecutiveRecalibration);
		consecutiveRecalibration = true;
	}
	else
	{
		consecutiveRecalibration = false;
	}

	VIOState newX = x; // set newX to last x

	//if the camera moves more than the minimum START distance
	//start the motion estimate
	if(avgFeatureChange > this->MIN_START_DIST)
	{
		this->started = true; // this is the initialize step

		//run ekf predict step.
		//this will update the state using imu measurements
		//it will also propagate the error throughout the predction step into the states covariance matrix
		newX = ekf.predict(x, currentFrame.timeImageCreated);


	}
	else
	{
		std::vector<sensor_msgs::Imu> newBuff;

		// empty the imu buffer
		for(int i = 0; i < this->ekf.imuMessageBuffer.size(); i++)
		{
			if(this->ekf.imuMessageBuffer.at(i).header.stamp.toSec() >= currentFrame.timeImageCreated.toSec())
			{
				newBuff.push_back(this->ekf.imuMessageBuffer.at(i));
			}
		}

		this->ekf.imuMessageBuffer = newBuff; // replace the buffer
	}

	return newX;
}



/*
 * uses epipolar geometry from two frames to
 * estimate relative motion of the frame;
 */
bool VIO::visualMotionInference(Frame frame1, Frame frame2, tf::Vector3 angleChangePrediction,
		tf::Vector3& rotationInference, tf::Vector3& unitVelocityInference, double& averageMovement)
{
	//first get the feature deltas from the two frames
	std::vector<cv::Point2f> prevPoints, currentPoints;
	feature_tracker.getCorrespondingPointsFromFrames(frame1, frame2, prevPoints, currentPoints);

	//undistort points using fisheye model
	//cv::fisheye::undistortPoints(prevPoints, prevPoints, this->K, this->D);
	//cv::fisheye::undistortPoints(currentPoints, currentPoints, this->K, this->D);

	//get average movement bewteen images
	averageMovement = feature_tracker.averageFeatureChange(prevPoints, currentPoints);

	//ensure that there are enough points to estimate motion with vo
	if(currentPoints.size() < 5)
	{
		return false;
	}

	cv::Mat mask;

	//calculate the essential matrix
	cv::Mat essentialMatrix = cv::findEssentialMat(prevPoints, currentPoints, this->K, cv::RANSAC, 0.999, 1.0, mask);

	//ensure that the essential matrix is the correct size
	if(essentialMatrix.rows != 3 || essentialMatrix.cols != 3)
	{
		return false;
	}

	//recover pose change from essential matrix
	cv::Mat translation;
	cv::Mat rotation;

	//decompose matrix to get possible deltas
	cv::recoverPose(essentialMatrix, prevPoints, currentPoints, this->K, rotation, translation, mask);


	//set the unit velocity inference
	unitVelocityInference.setX(translation.at<double>(0, 0));
	unitVelocityInference.setY(translation.at<double>(1, 0));
	unitVelocityInference.setZ(translation.at<double>(2, 0));

	return true;
}

/*
 * map average pixel change from zero to threshold and then make that a value from 0 to 1
 * So, now you have this value if you multiply it times velocity
 */
void VIO::recalibrateState(double avgPixelChange, double threshold, bool consecutive)
{
	//ROS_DEBUG_STREAM("recalibrating with " << avgPixelChange);

	static double lastNormalize = 0;
	static sensor_msgs::Imu lastImu;
	double normalize = avgPixelChange/threshold;
	sensor_msgs::Imu currentImu = ekf.getMostRecentImu();

	//ROS_DEBUG_STREAM("normalized pixel change " << normalize);

	state.setVelocity(normalize * state.getVelocity());
	//state.setVelocity(0 * state.getVelocity());

	//TODO make a gyro bias measurment vector in the inertial motion estimator and do a weighted average

	gyroNode gNode;
	gNode.gyroBias.setX(currentImu.angular_velocity.x);
	gNode.gyroBias.setY(currentImu.angular_velocity.y);
	gNode.gyroBias.setZ(currentImu.angular_velocity.z);
	gNode.certainty = (1-normalize);
	if(gyroQueue.size() >= DEFAULT_QUEUE_SIZE)
	{
		gyroQueue.pop_back();
		gyroQueue.insert(gyroQueue.begin(), gNode);
	}
	else
	{
		gyroQueue.insert(gyroQueue.begin(), gNode);
	}

	double gyroCertaintySum = 0;
	for(int i=0; i<gyroQueue.size(); ++i)
	{
		gyroCertaintySum += gyroQueue.at(i).certainty;
	}
	std::vector<double> gyroNormlizedCertainty;
	for(int i=0; i<gyroQueue.size(); ++i)
	{
		gyroNormlizedCertainty.push_back(gyroQueue.at(i).certainty / gyroCertaintySum);
	}

	//FINAL WEIGHTED GYROBIASES
	gyroNode gWeightedNode;
	gWeightedNode.gyroBias.setX(0);
	gWeightedNode.gyroBias.setY(0);
	gWeightedNode.gyroBias.setZ(0);
	gWeightedNode.certainty = 0;
	for(int i=0; i<gyroQueue.size(); ++i)
	{
		gWeightedNode.gyroBias.setX(gWeightedNode.gyroBias.getX() + gyroNormlizedCertainty.at(i)*gyroQueue.at(i).gyroBias.getX());
		gWeightedNode.gyroBias.setY(gWeightedNode.gyroBias.getY() + gyroNormlizedCertainty.at(i)*gyroQueue.at(i).gyroBias.getY());
		gWeightedNode.gyroBias.setZ(gWeightedNode.gyroBias.getZ() + gyroNormlizedCertainty.at(i)*gyroQueue.at(i).gyroBias.getZ());
	}

	ekf.gyroBiasX = gWeightedNode.gyroBias.getX();
	ekf.gyroBiasY = gWeightedNode.gyroBias.getY();
	ekf.gyroBiasZ = gWeightedNode.gyroBias.getZ();


	//POTENTIAL BUG
	if(consecutive)
	{
		normalize = (normalize+lastNormalize)/2;

		ROS_DEBUG_STREAM("running consecutive calibration with new normalized " << normalize);

		tf::Vector3 accel(lastImu.linear_acceleration.x*ekf.scaleAccelerometer
				, lastImu.linear_acceleration.y*ekf.scaleAccelerometer
				, lastImu.linear_acceleration.z*ekf.scaleAccelerometer);
		double scale = accel.length();

		//Vector with size DEFAULT_QUEUE_SIZE, elements added at front and dequeued at back
		accelNode aNode;
		aNode.certainty = (1-normalize);
		aNode.accelScale = GRAVITY_MAG/scale;
		if(accelQueue.size() >= DEFAULT_QUEUE_SIZE)
		{
			accelQueue.pop_back();
			accelQueue.insert(accelQueue.begin(), aNode);

		}
		else
		{
			accelQueue.insert(accelQueue.begin(), aNode);
		}
		//Calculating weighted values of gyroBiases and scale
		double accelCertaintySum = 0;
		//		queueNode WeightedValues;
		//		WeightedValues.gyroBias.setX(0);
		//		WeightedValues.gyroBias.setY(0);
		//		WeightedValues.gyroBias.setZ(0);
		//		WeightedValues.certainty = 0;
		//		WeightedValues.scale = 0;
		for(int i=0; i<accelQueue.size(); ++i)
		{
			accelCertaintySum += accelQueue.at(i).certainty;
			//			sum.gyroBias.setX(queue.at(i).gyroBias.getX()+sum.gyroBias.getX());
			//			sum.gyroBias.setY(queue.at(i).gyroBias.getY()+sum.gyroBias.getY());
			//			sum.gyroBias.setZ(queue.at(i).gyroBias.getZ()+sum.gyroBias.getZ());
			//			sum.certainty += queue.at(i).certainty;
			//			sum.scale += queue.at(i).scale;//sum += queue.at(i).certainty;//queue.at(i).scale;
		}

		std::vector<double> accelNormalizedCertainty;
		for(int i=0; i<accelQueue.size(); ++i)
		{
			accelNormalizedCertainty.push_back(accelQueue.at(i).certainty / accelCertaintySum);
			//			Node.certainty = queue.at(i).certainty / sum.certainty;
			//			Node.gyroBias.setX(queue.at(i).gyroBias.getX() / sum.gyroBias.getX());
			//			Node.gyroBias.setY(queue.at(i).gyroBias.getY() / sum.gyroBias.getY());
			//			Node.gyroBias.setZ(queue.at(i).gyroBias.getZ() / sum.gyroBias.getZ());
			//			Node.scale = queue.at(i).scale / sum.scale;
			//
			//			weigthedQueue.push_back(Node);
		}

		//FINAL WEIGHTED ACCELERATION SCALE
		accelNode aWeightedNode;
		aWeightedNode.certainty = 0;
		aWeightedNode.accelScale = 0;

		for(int i=0; i<accelQueue.size(); ++i)
		{
			aWeightedNode.accelScale += accelNormalizedCertainty.at(i)*accelQueue.at(i).accelScale;
			//			WeightedValues.gyroBias.setX(WeightedValues.gyroBias.getX() + normalizedCertainty.at(i)*queue.at(i).gyroBias.getX());
			//			WeightedValues.gyroBias.setY(WeightedValues.gyroBias.getY() + normalizedCertainty.at(i)*queue.at(i).gyroBias.getY());
			//			WeightedValues.gyroBias.setZ(WeightedValues.gyroBias.getZ() + normalizedCertainty.at(i)*queue.at(i).gyroBias.getZ());
			//			WeightedValues.scale += normalizedCertainty.at(i)*queue.at(i).scale;
		}
		//sum *= GRAVITY_MAG/queue.size();
		//TODO create a ten element running wieghted average of the accelerometer scale.
		if(scale != 0)
			ekf.scaleAccelerometer = aWeightedNode.accelScale; // + (normalize)*ekf.scaleAccelerometer;

		tf::Vector3 gravity(0,0,GRAVITY_MAG);

		correctOrientation(ekf.getDifferenceQuaternion(gravity, accel), (1-normalize));

		//ROS_DEBUG_STREAM("new acceleration after scaling " << ekf.scaleAccelerometer * accel);
	}

	//ROS_DEBUG_STREAM("new accel scale " << ekf.scaleAccelerometer << " new gyro biases " << ekf.gyroBiasX << ", " << ekf.gyroBiasY << ", " << ekf.gyroBiasZ);


	lastImu = currentImu;
	lastNormalize = normalize;
	return;
}




