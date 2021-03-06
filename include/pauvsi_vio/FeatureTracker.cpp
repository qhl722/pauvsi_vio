/*
 * FeatureTracker.cpp
 *
 *  Created on: Oct 18, 2016
 *      Author: kevinsheridan
 */

#include "FeatureTracker.h"

FeatureTracker::FeatureTracker()
{

}

void FeatureTracker::setParams(int fst, float mev, bool kbd, int nf, int mnfd)
{
	this->FEATURE_SIMILARITY_THRESHOLD = fst;
	this->MIN_EIGEN_VALUE = mev;
	this->KILL_BY_DISSIMILARITY = kbd;
	this->NUM_FEATURES = nf;
	this->MIN_NEW_FEATURE_DISTANCE = mnfd;
}

/*
 * This will match feature descriptors between two images
 *
 * In the first variant of this method, the train descriptors are passed as an input argument. In the
 * second variant of the method, train descriptors collection that was set by DescriptorMatcher::add is
 * used. Optional mask (or masks) can be passed to specify which query and training descriptors can be
 * matched. Namely, queryDescriptors[i] can be matched with trainDescriptors[j] only if
 * mask.at\<uchar\>(i,j) is non-zero.
 */
std::vector<cv::DMatch> FeatureTracker::matchFeaturesWithFlann(cv::Mat query, cv::Mat train){
	std::vector<cv::DMatch> matches;
	cv::FlannBasedMatcher matcher(new cv::flann::LshIndexParams(20, 10, 2));
	matcher.match(query, train, matches);

	ROS_DEBUG_STREAM_THROTTLE(2, "query size: " << query.rows << " train size: " << train.rows << " matches size: " << matches.size());

	return matches;
}

/*
 * uses optical flow to find a vector of features in another image
 * This function does not require a prediction
 * This will set the feature vector within the new frame with the
 * flowed points
 */
bool FeatureTracker::flowFeaturesToNewFrame(Frame& oldFrame, Frame& newFrame){

	std::vector<cv::Point2f> oldPoints = oldFrame.getPoint2fVectorFromFeatures();
	//ROS_DEBUG_STREAM_ONCE("got " << oldPoints.size() << " old point2fs from the oldframe which has " << oldFrame.features.size() << " features");
	std::vector<cv::Point2f> newPoints;

	std::vector<uchar> status; // status vector for each point
	cv::Mat error; // error vector for each point

	//ROS_DEBUG_ONCE("running lucas kande optical flow algorithm");
	/*
	 * this calculates the new positions of the old features in the new image
	 * status tells us whether or not a point index has been flowed over to the new frame
	 * last value is a minimum eigen value thresh
	 * it will kill bad features
	 */
	ros::Time t_start = ros::Time::now();
	cv::calcOpticalFlowPyrLK(oldFrame.image, newFrame.image, oldPoints, newPoints, status, error, cv::Size(21, 21), 3,
			cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS, 30, 0.01), 0, this->MIN_EIGEN_VALUE);
	ROS_DEBUG_STREAM("ran flow in :" << 1000 * (ros::Time::now().toSec() - t_start.toSec()));

	//ROS_DEBUG_STREAM_ONCE("ran optical flow and got " << newPoints.size() << " points out");

	int lostFeatures = 0;
	//next add these features into the new Frame
	for (int i = 0; i < newPoints.size(); i++)
	{
		//check if the point was able to flow
		if(status.at(i) == 1)
		{
			ROS_DEBUG("creating new feature from flowed feat");
			// the id number is not that important because it will be handled by the frame
			Feature feat(&newFrame, newPoints.at(i), oldFrame.features.at(i).point); // create a matched feature with id = -1
			ROS_DEBUG_STREAM("frame index: " << feat.frame);
			//ROS_ASSERT(oldFrame.features.at(i).point == feat.point);
			//ROS_ASSERT(oldFrame.features.at(i).point->observations.size() == feat.point->observations.size());

			//if the previous feature was described
			if(oldFrame.features.at(i).described)
			{
				//ROS_DEBUG("transferring feature description");
				feat.description = (oldFrame.features.at(i).description); // transfer previous description to new feature
				//ROS_DEBUG_STREAM_THROTTLE(0, feat.getFeatureDescription());
			}

			//ROS_DEBUG_STREAM("old: " << oldFrame.features.at(i).getMatchedIDDeque().size() << " new: " << feat.getMatchedIDDeque().size());

			//find the cause of this bug and remove this hacky fix
			// i have to reset the last observations pointer for some reason
			//feat.point->observations.at(1) = &oldFrame.features.at(i);
			newFrame.addFeature(feat); // add this feature to the new frame
			newFrame.features.back().point->observations.at(0) = &newFrame.features.back(); // i must refer the feature
			ROS_DEBUG("finished adding feature");

			/*ROS_ASSERT(&newFrame.features.at(newFrame.features.size() - 1) == &newFrame.features.back());

			//assert to check that everything checks out
			ROS_ASSERT(*(newFrame.features.back().frame) == newFrame);
			ROS_ASSERT(*(newFrame.features.back().point->observations.at(0)->frame) == newFrame);
			ROS_DEBUG_STREAM("oldFrame feature: " << &oldFrame.features.at(i) << " point feature pointer: " << newFrame.features.back().point->observations.at(1));
			ROS_DEBUG_STREAM((newFrame.features.back().point->observations.at(1)->frame));
			ROS_DEBUG_STREAM("old frame address: " << &oldFrame);
			ROS_DEBUG_STREAM((newFrame.features.back().point->observations.at(0)->frame));
			ROS_DEBUG_STREAM("new frame address: " << &newFrame);

			if(newFrame.features.back().point->observations.size() > 2)
			{
				ROS_DEBUG_STREAM("### third frame pointer " << newFrame.features.back().point->observations.at(2)->frame);
			}

			ROS_ASSERT(*(newFrame.features.back().point->observations.at(1)->frame) == oldFrame);


			//set the forward match for the old feature
			oldFrame.features.at(i).forwardMatched = true;
			oldFrame.features.at(i).forwardMatchIndex = newFrame.features.size() - 1;
			oldFrame.features.at(i).forwardMatchID = newFrame.features.at(newFrame.features.size() - 1).getFeatureID();*/
		}
		else
		{
			lostFeatures++;
			//oldFrame.features.at(i).point->setStatus(Point::TRACKING_LOST); // update the status of the point to lost this will then be cleaned up
			ROS_ASSERT(oldFrame.features.at(i).point != NULL);
			ROS_DEBUG("deleting point");
			oldFrame.features.at(i).point->safelyDelete();
			ROS_DEBUG("finished deleting point");
		}
	}
	ROS_DEBUG("at end of optical flow");
	ROS_DEBUG_STREAM_COND(lostFeatures, "optical flow lost " << lostFeatures <<  " feature(s)");

	//if user wants to kill by similarity
	if(KILL_BY_DISSIMILARITY)
	{
		//ROS_DEBUG("killing by similarity");
		this->checkFeatureConsistency(newFrame, this->FEATURE_SIMILARITY_THRESHOLD);
	}

	return true;
}



/*
 * gets corresponding points between the two frames as two vectors of point2f
 * checks if index and id match for saftey
 */
/*
void FeatureTracker::getCorrespondingPointsFromFrames(Frame lastFrame, Frame currentFrame, std::vector<cv::Point2f>& lastPoints, std::vector<cv::Point2f>& currentPoints)
{

	for (int i = 0; i < currentFrame.features.size(); i++)
	{
		if(currentFrame.features.at(i).isMatched() &&
				lastFrame.features.at(currentFrame.features.at(i).getMatchedIndex()).getFeatureID() ==
						currentFrame.features.at(i).getMatchedID()){
			lastPoints.push_back(lastFrame.features.at(currentFrame.features.at(i).getMatchedIndex()).getFeature().pt);
			currentPoints.push_back(currentFrame.features.at(i).getFeature().pt);
		}
		else
		{
			ROS_WARN("could not match feature id to index");
		}
	}
}/*

/*
 * checks to see if current descriptor is similar to actual feature
 * if similarity is bellow threshold, feature is kept and descriptor is updated
 * otherwise feature is removed from feature vector
 */
void FeatureTracker::checkFeatureConsistency(Frame& checkFrame, int killThreshold ){
	cv::Mat newDescription = checkFrame.describeFeaturesWithBRIEF(checkFrame.image, checkFrame.features);

	std::vector<Feature> tempFeatures;

	for (int i = 0; i < checkFrame.features.size(); i++){

		if(!checkFrame.features.at(i).described)
			break;

		cv::Mat row = newDescription.row(i);

		//ROS_DEBUG_STREAM_ONCE("got feature description " << row);

		int x = checkFrame.compareDescriptors(row, checkFrame.features.at(i).description);
		//int x = checkFrame.compareDescriptors(row, row);

		if (x <= killThreshold){

			//ROS_DEBUG_STREAM("features match " << i <<" : "<<checkFrame.features.size()<<" : "<< newDescription.rows <<" : " << x);
			//ROS_DEBUG_STREAM("i+1: "<< checkFrame.features.at(i+1).getFeatureDescription()<<":"<<checkFrame.features.at(i+1).isFeatureDescribed());
			//ROS_DEBUG_STREAM("description size " << checkFrame.features.at(i).getFeatureDescription().cols);

			checkFrame.features.at(i).description = row;

			//ROS_DEBUG("modified feature");

			tempFeatures.push_back(checkFrame.features.at(i));

			//ROS_DEBUG("pushed back modified feature");
		}
		else{
			//checkFrame.features.at(i).point->setStatus(Point::TRACKING_LOST); // set the tracking to lost
			ROS_DEBUG("feature does'nt match enough, killing");
		}
	}

	//ROS_DEBUG("setting new features");
	checkFrame.features = tempFeatures;
	//ROS_DEBUG("set new features");
}


/*
 * find the average change in position
 * for all feature correspondences
 * vectors must be same sizes
 */
double FeatureTracker::averageFeatureChange(std::vector<cv::Point2f> points1, std::vector<cv::Point2f> points2)
{
	double diff = 0;
	double dx, dy;
	for(int i = 0; i < points1.size(); i++)
	{
		dx = (double)(points1.at(i).x - points2.at(i).x);
		dy = (double)(points1.at(i).y - points2.at(i).y);
		diff += sqrt(pow(dx, 2) + pow(dy, 2));
	}

	return diff / (double)points1.size();
}

double FeatureTracker::averageFeatureChange(Frame& lf, Frame& cf)
{
	int numMatched = 0;
	double delta = 0;
	double dx, dy;

	for(int i = 0; i < cf.features.size(); i++)
	{
		if(cf.features.at(i).point->observations.size() >= 2)
		{
			//ROS_DEBUG_STREAM("from avgFeatChange: " << cf.features.at(i).point->observations.at(1)->frame);
			cv::Point2f p1 = cf.features.at(i).point->observations.at(1)->undistort_pxl;
			ROS_ASSERT(cf.features.at(i).undistorted);
			cv::Point2f p2 = cf.features.at(i).undistort_pxl;
			//ROS_DEBUG_STREAM("undistorted px: " << p2 << " and " << p1);

			dx = (double)(p1.x - p2.x);
			dy = (double)(p1.y - p2.y);
			delta += sqrt(pow(dx, 2) + pow(dy, 2));
			numMatched++;
		}
	}

	return delta / (double)numMatched;
}




