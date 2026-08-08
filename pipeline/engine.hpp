#pragma once
#include <string>
#include <vector>
#include <cuda_runtime.h>

namespace nvinfer1 { class ICudaEngine; class IExecutionContext; class IRuntime; }

// TensorRT 10.x wrapper. Shapes are static (the ONNX is frozen beforehand), so
// there is no optimisation profile and no per-request shape setting -- which is
// exactly what removes the latency jitter TensorRT otherwise shows on Orin.
class Engine {
public:
    ~Engine();

    // Loads `enginePath` if present and compatible; otherwise builds from
    // `onnxPath` and writes the plan. Engine building takes minutes and must
    // never happen on a request path.
    bool loadOrBuild(const std::string& onnxPath, const std::string& enginePath,
                     bool fp16, size_t workspaceMB);

    // One execution context per stream.
    bool createContexts(int n);

    // Binds the two tensors and enqueues. Non-blocking.
    bool enqueue(int ctxIdx, void* dIn, void* dOut, cudaStream_t s);

    int batch()   const { return batch_; }
    int imgsz()   const { return imgsz_; }
    int channels() const { return channels_; }
    int outChannels() const { return outC_; }   // expected 6
    int anchors() const { return outA_; }       // expected 8400 at 640

    size_t inputElems()  const { return size_t(batch_) * channels_ * imgsz_ * imgsz_; }
    size_t outputElems() const { return size_t(batch_) * outC_ * outA_; }

    const std::string& inputName()  const { return inName_; }
    const std::string& outputName() const { return outName_; }

private:
    nvinfer1::IRuntime* runtime_ = nullptr;
    nvinfer1::ICudaEngine* engine_ = nullptr;
    std::vector<nvinfer1::IExecutionContext*> ctx_;

    std::string inName_, outName_;
    int batch_ = 0, channels_ = 0, imgsz_ = 0;
    int outC_ = 0, outA_ = 0;

    bool build(const std::string& onnxPath, const std::string& enginePath,
               bool fp16, size_t workspaceMB);
    bool introspect();
};
