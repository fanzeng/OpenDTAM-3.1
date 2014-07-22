// Free for non-commercial, non-military, and non-critical 
// use unless incorporated in OpenCV. 
// Inherits OpenCV License if in OpenCV.


#include "CostVolume.hpp"
#include <opencv2/core/operations.hpp>
#include <opencv2/gpu/stream_accessor.hpp>
#include <opencv2/gpu/device/common.hpp>

#include "utils/utils.hpp"
#include "utils/tinyMat.hpp"
#include <iostream>


using namespace std;
using namespace cv;
using namespace gpu;



namespace cv { namespace gpu { namespace device { namespace dtam_updateCost{
    struct m33{
            float data[9];
        };
        struct m34{
            float data[12];
        };
    void loadConstants(int h_layers, int h_layerStep, float3* h_base,
            float* h_hdata, float* h_cdata, float* h_lo, float* h_hi, uint* h_loInd,
            uint h_rows, uint h_cols, cudaTextureObject_t h_tex);
    void updateCostColCaller(int cols,int rows, int y, m33 sliceToIm);
    void passThroughCaller(int cols,int rows);
    void perspCaller(int cols,int rows,m34 persp);
    void volumeProjectCaller(int cols,int rows,m34 p);
}}}}

void CostVolume::solveProjection(const cv::Mat& R, const cv::Mat& T) {
    Mat P;
    RTToP(R, T, P);
    cout<<cameraMatrix<<endl;
    projection.create(4, 4, CV_64FC1);
    projection=0.0;
    projection(Range(0, 2), Range(0, 3)) += cameraMatrix.rowRange(0, 2);

    projection.at<double>(2,3)=1.0;
    projection.at<double>(3,2)=1.0;
    cout<<"Augmented Camera Matrix:\n"<<projection<<endl;
    projection=projection*P;
    
    cout<<projection<<endl;
    
   // exit(0);
}

void CostVolume::checkInputs(const cv::Mat& R, const cv::Mat& T,
        const cv::Mat& _cameraMatrix) {
    assert(R.size() == Size(3, 3));
    assert(R.type() == CV_64FC1);
    assert(T.size() == Size(1, 3));
    assert(T.type() == CV_64FC1);
    assert(_cameraMatrix.size() == Size(3, 3));
    assert(_cameraMatrix.type() == CV_64FC1);
}

CostVolume::CostVolume(Mat image, FrameID _fid, int _layers, float _near,
        float _far, cv::Mat R, cv::Mat T, cv::Mat _cameraMatrix,
        float initialCost, float initialWeight) {
    checkInputs(R, T, _cameraMatrix);
    fid           = _fid;
    rows          = image.rows;
    cols          = image.cols;
    layers        = _layers;
    near          = _near;
    far           = _far;
    depthStep     = (near - far) / (layers - 1);
    cameraMatrix  = _cameraMatrix.clone();
    solveProjection(R, T);
    baseImage.upload(image);
    lo.create(image.size(), CV_32FC1);
    hi.create(image.size(), CV_32FC1);
    loInd.create(image.size(), CV_32FC1);
    dataContainer.create(layers, rows * cols, CV_32FC1);
    dataContainer = initialCost;
    data = (float*) dataContainer.data;
    hitContainer.create(layers, rows * cols, CV_32FC1);
    hitContainer = initialWeight;
    hits = (float*) hitContainer.data;
    count = 0;

}


class ArrayTexture {
public:
    int* refcount;
    int ref_count;
    cudaArray* cuArray;
    cudaTextureObject_t texObj;

    ArrayTexture(const cv::gpu::CudaMem& image, const Stream& cvStream =
            Stream::Null()) {
        refcount=&ref_count;
        ref_count=1;
        Mat im2=image.clone();
        assert(image.isContinuous());
        assert(image.type()==CV_8UC4);

        //Describe texture
        struct cudaTextureDesc texDesc;
        memset(&texDesc, 0, sizeof(texDesc));
        texDesc.addressMode[0]   = cudaAddressModeClamp;
        texDesc.addressMode[1]   = cudaAddressModeClamp;
        texDesc.filterMode       = cudaFilterModeLinear;
        texDesc.readMode         = cudaReadModeNormalizedFloat;
        texDesc.normalizedCoords = 0;
        cudaChannelFormatDesc channelDesc = //{8, 8, 8, 8, cudaChannelFormatKindUnsigned};
                       cudaCreateChannelDesc(8, 8, 8, 8, cudaChannelFormatKindUnsigned);
        //Fill Memory
        if(!cuArray){
            cudaSafeCall(cudaMallocArray(&cuArray, &channelDesc, image.cols, image.rows));
        }
        cudaSafeCall(cudaMemcpyToArray(cuArray, 0, 0, im2.datastart, im2.dataend-im2.datastart,
                                  cudaMemcpyHostToDevice/*,StreamAccessor::getStream(cvStream)*/));

        // Specify texture memory location
        struct cudaResourceDesc resDesc;
        memset(&resDesc, 0, sizeof(resDesc));
        resDesc.resType = cudaResourceTypeArray;
        resDesc.res.array.array = cuArray;

        // Create texture object
        cudaSafeCall(cudaCreateTextureObject(&texObj, &resDesc, &texDesc, NULL));
        cudaSafeCall(cudaDeviceSynchronize());
       
        
    }

    ArrayTexture& operator = (const ArrayTexture& tex) {
        if (this != &tex) {
            release();

            if (tex.refcount)
                CV_XADD(tex.refcount, 1);

            this->refcount=tex.refcount;
        }

        return *this;
    }

    void release() {
        if (refcount && CV_XADD(refcount, -1) == 1)
                deallocate();
    }

    void deallocate(){
        cudaDestroyTextureObject(texObj);
        cudaFreeArray(cuArray);

    }

    ~ArrayTexture() {
        release();
    }

};

cudaArray* cuArray;

cudaTextureObject_t simpleTex(Mat image){
    Mat im2=image.clone();
    assert(image.isContinuous());
    assert(image.type()==CV_8UC4);
    
    //Describe texture
    struct cudaTextureDesc texDesc;
    memset(&texDesc, 0, sizeof(texDesc));
    texDesc.addressMode[0]   = cudaAddressModeClamp;
    texDesc.addressMode[1]   = cudaAddressModeClamp;
    texDesc.filterMode       = cudaFilterModeLinear;
    texDesc.readMode         = cudaReadModeNormalizedFloat;
    texDesc.normalizedCoords = 0;
    cudaChannelFormatDesc channelDesc = //{8, 8, 8, 8, cudaChannelFormatKindUnsigned};
    cudaCreateChannelDesc<uchar4>();
    //Fill Memory
    cudaSafeCall(cudaMallocArray(&cuArray, &channelDesc, image.cols, image.rows));
    cudaSafeCall(cudaMemcpyToArray(cuArray, 0, 0, im2.datastart, im2.dataend-im2.datastart,
                                   cudaMemcpyHostToDevice/*,StreamAccessor::getStream(cvStream)*/));
    
    // Specify texture memory location
    struct cudaResourceDesc resDesc;
    memset(&resDesc, 0, sizeof(resDesc));
    resDesc.resType = cudaResourceTypeArray;
    resDesc.res.array.array = cuArray;
    
    cudaTextureObject_t texObj;
    // Create texture object
    cudaSafeCall(cudaCreateTextureObject(&texObj, &resDesc, &texDesc, NULL));
    return texObj;
}



void CostVolume::updateCost(const cv::gpu::CudaMem& image, const cv::Mat& R, const cv::Mat& T,const cv::gpu::Stream& cvStream){
    using namespace cv::gpu::device::dtam_updateCost;
    // 0  1  2  3
    // 4  5  6  7
    // 8  9  10 11
    // 12 13 14 15
    //
    // 0 1 2
    // 3 4 5
    // 6 7 8
    //
    // want cudaReadModeNormalizedFloat for auto convert to [0,1]
    // cudaAddressModeClamp
    // cudaFilterModeLinear
    //
    // make sure we modify the cameraMatrix to take into account the texture coordinates
    //


    //change input image to a texture
    //ArrayTexture tex(image, cvStream);
    cudaTextureObject_t texObj = simpleTex(image);
    cudaSafeCall( cudaDeviceSynchronize() );

    //find projection matrix from cost volume to image (3x4)
    Mat viewMatrixImage;
    RTToP(R,T,viewMatrixImage);
    Mat cameraMatrixTex(3,4,CV_64FC1);
    cameraMatrixTex=0.0;
    cameraMatrix.copyTo(cameraMatrixTex(Range(0,3),Range(0,3)));
    cameraMatrixTex(Range(0,2), Range(2,3)) += 0.5;//add 0.5 to x,y out //removing causes crash

    Mat imFromWorld=cameraMatrixTex*viewMatrixImage;//3x4
    Mat imFromCV=imFromWorld*projection.inv();
    //imFromCV.colRange(2,3)/=depthStep;
    //load up the constant stuff
    loadConstants(layers, rows*cols, (float3*) (baseImage.data),
            hits,  data, (float*) (lo.data), (float*) (hi.data), (uint*) (loInd.data),
            rows, cols,texObj);
    //for each slice
    for(int y=0; y<rows; y++){
        //find projection from slice to image (3x3)
        double *p = (double*)imFromCV.data;
        m33 sliceToIm={  p[0], p[2], p[3]+y*p[1],
                         p[4], p[6], p[7]+y*p[5],
                         p[8], p[10], p[11]+y*p[9]};



//        //kernel updates slice (1 block?)
//        updateCostColCaller(cols,1, y, sliceToIm);
//        passThroughCaller(cols,rows);
        m34 persp;
        for(int i=0;i<12;i++)
            persp.data[i]=p[i];
//        perspCaller(cols,rows,persp);
        volumeProjectCaller(cols,rows,persp);
//        cudaSafeCall( cudaDeviceSynchronize() );
            //per thread:
                //find projection from column to image (3x2)
                //for each pixel:
                    //finds L1 error
                    //blend in with old value
                    //if low
                        //update low index
                        //update high value
                    //if high
                        //update high value
                //save results
    }


}


