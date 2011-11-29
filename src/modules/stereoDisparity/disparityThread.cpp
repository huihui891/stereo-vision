#include "disparityThread.h"
#include <yarp/os/Stamp.h> 

void disparityThread::printMatrix(Mat &matrix) {
    int row=matrix.rows;
    int col =matrix.cols;
        cout << endl;
    for(int i = 0; i < matrix.rows; i++)
    {
        const double* Mi = matrix.ptr<double>(i);
        for(int j = 0; j < matrix.cols; j++)
            cout << Mi[j] << " ";
        cout << endl;
    }
        cout << endl;
}

disparityThread::disparityThread(string inputLeftPortName, string inputRightPortName, string outName, 
                                 string calibPath,Port* commPort)
{
    this->inputLeftPortName=inputLeftPortName;
    this->inputRightPortName=inputRightPortName;
    this->outName=outName;
    this->commandPort=commPort;
    this->stereo=new StereoCamera(calibPath+"/intrinsics.yml", calibPath+"/extrinsics.yml");
    angle=0;
    this->mutexDisp = new Semaphore(1);
    this->HL_root= Mat::zeros(4,4,CV_64F);
    this->output=NULL;
}



bool disparityThread::threadInit() 
{
    if (!imagePortInLeft.open(inputLeftPortName.c_str())) {
        cout  << ": unable to open port " << inputLeftPortName << endl;
        return false; 
    }

    if (!imagePortInRight.open(inputRightPortName.c_str())) {
        cout << ": unable to open port " << inputRightPortName << endl;
        return false;
    }

    if (!outPort.open(outName.c_str())) {
        cout << ": unable to open port " << outName << endl;
        return false;
    }

    Property option;
    option.put("device","gazecontrollerclient");
    option.put("remote","/iKinGazeCtrl");
    option.put("local","/client/gaze");
    gazeCtrl=new PolyDriver(option);
    if (gazeCtrl->isValid()) {
        gazeCtrl->view(igaze);
    }
    else {
        cout<<"Devices not available"<<endl;
        return false;
    }

    return true;
}

void disparityThread::printMatrixYarp(Matrix &A) {
    cout << endl;
    for (int i=0; i<A.rows(); i++) {
        for (int j=0; j<A.cols(); j++) {
            cout<<A(i,j)<<" ";
        }
        cout<<endl;
    }
    cout << endl;
}

void disparityThread::convert(Matrix& matrix, Mat& mat) {
    mat=cv::Mat(matrix.rows(),matrix.cols(),CV_64FC1);
    for(int i=0; i<matrix.rows(); i++)
        for(int j=0; j<matrix.cols(); j++)
            mat.at<double>(i,j)=matrix(i,j);
}

void disparityThread::convert(Mat& mat, Matrix& matrix) {
    matrix.resize(mat.rows,mat.cols);
    for(int i=0; i<mat.rows; i++)
        for(int j=0; j<mat.cols; j++)
            matrix(i,j)=mat.at<double>(i,j);
}

Matrix disparityThread::getCameraH(int camera) {

    yarp::sig::Vector x_curr;
    yarp::sig::Vector o_curr;

    if(camera==LEFT)
        igaze->getLeftEyePose(x_curr, o_curr);
    else
        igaze->getRightEyePose(x_curr, o_curr);

    Matrix R_curr=axis2dcm(o_curr);

    Matrix H_curr(4, 4);
    for (int i=0; i<R_curr.cols(); i++)
        H_curr.setCol(i, R_curr.getCol(i));
    for (int i=0; i<x_curr.size(); i++)
        H_curr(i,3)=x_curr[i];


    if(camera==LEFT)
    {
        this->mutexDisp->wait();
        convert(H_curr,HL_root);
        this->mutexDisp->post();
    }


    return H_curr;
}

void disparityThread::run(){

    imageL=new ImageOf<PixelRgb>;
    imageR=new ImageOf<PixelRgb>;

    bool initL=false;
    bool initR=false;


    Matrix yarp_initLeft,yarp_initRight;
    Matrix yarp_H0;

    Point3d point;
    bool init=true;
    while (!isStopping()) {

        imageL = imagePortInLeft.read(false);
        imageR = imagePortInRight.read(false);

        if(imageL!=NULL && imageR!=NULL){
            imgL= (IplImage*) imageL->getIplImage();
            imgR= (IplImage*) imageR->getIplImage();

            this->stereo->setImages(imgL,imgR);

            if(init) {
                stereo->undistortImages();
                stereo->findMatch();
                stereo->estimateEssential();
                stereo->hornRelativeOrientations();
                output=cvCreateImage(cvSize(imgL->width,imgL->height),8,3);
                init=false;

                Mat H0_R=this->stereo->getRotation();
                Mat H0_T=this->stereo->getTranslation();

                // get the initial camera relative position
                Mat H0=buildRotTras(H0_R,H0_T);

                convert(H0,yarp_H0);

                //get the initial left and right positions
                yarp_initLeft=getCameraH(LEFT);
                yarp_initRight=getCameraH(RIGHT);
            }

                //transformation matrices between prev and curr eye frames
                Matrix yarp_Left=getCameraH(LEFT);
                Matrix yarp_Right=getCameraH(RIGHT);

                yarp_Left=SE3inv(yarp_Left)*yarp_initLeft;
                yarp_Right=SE3inv(yarp_Right)*yarp_initRight;

                Matrix Hcurr=yarp_Right*yarp_H0*SE3inv(yarp_Left);

                Matrix R=Hcurr.submatrix(0,2,0,2);
                yarp::sig::Vector x=dcm2axis(R);
                Matrix newTras=Hcurr.submatrix(0,2,3,3);

                this->mutexDisp->wait();
                // Update Rotation
                Mat Rot(3,3,CV_64FC1);
                convert(R,Rot);
                this->stereo->setRotation(Rot,0);

                //Update Translation
                Mat translation(3,1,CV_64FC1);
                convert(newTras,translation);
                this->stereo->setTranslation(translation,0);
                this->stereo->computeDisparity();

                this->mutexDisp->post();

                if(outPort.getOutputCount()>0) {
                    disp=stereo->getDisparity();
                    cvCvtColor(&disp,output,CV_GRAY2RGB);
                    ImageOf<PixelBgr>& outim=outPort.prepare();
                    outim.wrapIplImage(output);
                    outPort.write();
                }

                initL=initR=false;
        }

    }
}


void disparityThread::threadRelease() 
{
    imagePortInRight.close();
    imagePortInLeft.close();
    outPort.close();
    commandPort->close();
    delete this->stereo;
    delete this->mutexDisp;
    delete gazeCtrl;

    if(output!=NULL)
        cvReleaseImage(&output);

}
void disparityThread::onStop() {
    imagePortInRight.interrupt();
    imagePortInLeft.interrupt();
    commandPort->interrupt();
}

Mat disparityThread::buildRotTras(Mat & R, Mat & T) {
        
        Mat A = Mat::eye(4, 4, CV_64F);
        for(int i = 0; i < R.rows; i++)
         {
         double* Mi = A.ptr<double>(i);
         double* MRi = R.ptr<double>(i);
            for(int j = 0; j < R.cols; j++)
                 Mi[j]=MRi[j];
         }
        for(int i = 0; i < T.rows; i++)
         {
         double* Mi = A.ptr<double>(i);
         double* MRi = T.ptr<double>(i);
                 Mi[3]=MRi[0];
         }
        return A;
}


Point3f disparityThread::get3DPoints(int u, int v, string drive) {
    u=u; // matrix starts from (0,0), pixels from (0,0)
    v=v;
    Point3f point;

    if(drive!="RIGHT" && drive !="LEFT" && drive!="ROOT") {
        point.x=-1.0;
        point.y=-1.0;
        point.z=-1.0;
        return point;
    }

    this->mutexDisp->wait();   
    // Mapping from Rectified Cameras to Original Cameras
    Mat Mapper=this->stereo->getMapperL();

    if(Mapper.empty()) {
        point.x=-1.0;
        point.y=-1.0;
        point.z=-1.0;

        this->mutexDisp->post();   
        return point;
    }


    float usign=Mapper.ptr<float>(v)[2*u];
    float vsign=Mapper.ptr<float>(v)[2*u+1]; 

    u=cvRound(usign);
    v=cvRound(vsign);

    IplImage disp16=this->stereo->getDisparity16();


    if(u<0 || u>=disp.width || v<0 || v>=disp.height) {
        point.x=-1.0;
        point.y=-1.0;
        point.z=-1.0;
    }
    else {
        Mat Q=this->stereo->getQ();
        CvScalar scal= cvGet2D(&disp16,v,u);
        double disparity=-scal.val[0]/16.0;
        float w= (float) ((float) disparity*Q.at<double>(3,2)) + ((float)Q.at<double>(3,3));
        point.x= (float)((float) (usign+1)*Q.at<double>(0,0)) + ((float) Q.at<double>(0,3));
        point.y=(float)((float) (vsign+1)*Q.at<double>(1,1)) + ((float) Q.at<double>(1,3));
        point.z=(float) Q.at<double>(2,3);

        point.x=point.x/w;
        point.y=point.y/w;
        point.z=point.z/w;
    }

    // discard points far more than 2.5 meters or with not valid disparity (<0)
    if(point.z>2.5 || point.z<0) {
        point.x=-1.0;
        point.y=-1.0;
        point.z=-1.0;
    } 
    else {
       if(drive=="LEFT") {
            Mat P(3,1,CV_64FC1);
            P.at<double>(0,0)=point.x;
            P.at<double>(1,0)=point.y;
            P.at<double>(2,0)=point.z;

            P=this->stereo->getRLrect().t()*P;

            point.x=(float) P.at<double>(0,0);
            point.y=(float) P.at<double>(1,0);
            point.z=(float) P.at<double>(2,0);
       }
       if(drive=="RIGHT") {
            Mat Rright = this->stereo->getRotation();
            Mat Tright = this->stereo->getTranslation();
            Mat RRright = this->stereo->getRRrect().t();
            Mat TRright = Mat::zeros(0,3,CV_64F);

            Mat HRL=buildRotTras(Rright,Tright);
            Mat Hrect=buildRotTras(RRright,TRright);

            Mat P(4,1,CV_64FC1);
            P.at<double>(0,0)=point.x;
            P.at<double>(1,0)=point.y;
            P.at<double>(2,0)=point.z;
            P.at<double>(3,0)=1;
           
            P=Hrect*HRL*P;     

            point.x=(float) ((float) P.at<double>(0,0)/P.at<double>(3,0));
            point.y=(float) ((float) P.at<double>(1,0)/P.at<double>(3,0));
            point.z=(float) ((float) P.at<double>(2,0)/P.at<double>(3,0));

        }

        if(drive=="ROOT") {
            Mat RLrect=this->stereo->getRLrect().t();
            Mat Tfake = Mat::zeros(0,3,CV_64F);
            Mat P(4,1,CV_64FC1);
            P.at<double>(0,0)=point.x;
            P.at<double>(1,0)=point.y;
            P.at<double>(2,0)=point.z;
            P.at<double>(3,0)=1;

            Mat Hrect=buildRotTras(RLrect,Tfake);
            P=HL_root*Hrect*P;
            point.x=(float) ((float) P.at<double>(0,0)/P.at<double>(3,0));
            point.y=(float) ((float) P.at<double>(1,0)/P.at<double>(3,0));
            point.z=(float) ((float) P.at<double>(2,0)/P.at<double>(3,0));
       }



    }

    this->mutexDisp->post();
    return point;

}
