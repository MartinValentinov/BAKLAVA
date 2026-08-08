#include "engine.hpp"
#include "util.hpp"

#include <NvInfer.h>
#include <NvOnnxParser.h>

#include <fstream>
#include <memory>
#include <cstring>

using namespace nvinfer1;

namespace {

class Logger : public ILogger {
public:
    void log(Severity s, const char* msg) noexcept override {
        if (s <= Severity::kWARNING)
            std::fprintf(stderr, "[trt] %s\n", msg);
    }
};
Logger gLogger;

std::vector<char> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamsize n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<size_t>(n));
    if (!f.read(buf.data(), n)) return {};
    return buf;
}

}  // namespace

Engine::~Engine() {
    for (auto* c : ctx_) delete c;
    delete engine_;
    delete runtime_;
}

bool Engine::loadOrBuild(const std::string& onnxPath, const std::string& enginePath,
                         bool fp16, size_t workspaceMB) {
    std::vector<char> plan = readFile(enginePath);

    if (plan.empty()) {
        std::fprintf(stderr, "[trt] no cached plan at %s -- building "
                             "(this takes minutes; do it offline)\n", enginePath.c_str());
        if (!build(onnxPath, enginePath, fp16, workspaceMB)) return false;
        plan = readFile(enginePath);
        if (plan.empty()) return false;
    }

    runtime_ = createInferRuntime(gLogger);
    if (!runtime_) return false;

    engine_ = runtime_->deserializeCudaEngine(plan.data(), plan.size());
    if (!engine_) {
        std::fprintf(stderr, "[trt] deserialize failed. A plan is tied to the exact "
                             "GPU and TensorRT version -- delete %s and rebuild.\n",
                     enginePath.c_str());
        return false;
    }
    return introspect();
}

bool Engine::build(const std::string& onnxPath, const std::string& enginePath,
                   bool fp16, size_t workspaceMB) {
    auto* builder = createInferBuilder(gLogger);
    if (!builder) return false;

    // Static shapes: no kEXPLICIT_BATCH flag needed in TRT 10, no profile.
    auto* network = builder->createNetworkV2(0);
    auto* parser  = nvonnxparser::createParser(*network, gLogger);

    if (!parser->parseFromFile(onnxPath.c_str(),
                               static_cast<int>(ILogger::Severity::kWARNING))) {
        std::fprintf(stderr, "[trt] ONNX parse failed\n");
        for (int i = 0; i < parser->getNbErrors(); ++i)
            std::fprintf(stderr, "[trt]   %s\n", parser->getError(i)->desc());
        return false;
    }

    // Reject a dynamic model early with a clear message rather than letting
    // TensorRT fail deep inside the builder.
    for (int i = 0; i < network->getNbInputs(); ++i) {
        Dims d = network->getInput(i)->getDimensions();
        for (int j = 0; j < d.nbDims; ++j) {
            if (d.d[j] < 0) {
                std::fprintf(stderr,
                    "[trt] input '%s' still has a dynamic axis. Freeze the ONNX first:\n"
                    "      python3 04_freeze_onnx_shapes.py model.onnx -o frozen.onnx "
                    "--batch N --imgsz 640\n", network->getInput(i)->getName());
                return false;
            }
        }
    }

    auto* config = builder->createBuilderConfig();
    config->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, workspaceMB << 20u);
    if (fp16 && builder->platformHasFastFp16()) {
        config->setFlag(BuilderFlag::kFP16);
        std::fprintf(stderr, "[trt] FP16 enabled\n");
    }
    config->setBuilderOptimizationLevel(3);

    IHostMemory* serialized = builder->buildSerializedNetwork(*network, *config);
    if (!serialized) { std::fprintf(stderr, "[trt] build failed\n"); return false; }

    std::ofstream out(enginePath, std::ios::binary);
    out.write(static_cast<const char*>(serialized->data()), serialized->size());
    out.close();

    delete serialized;
    delete config;
    delete parser;
    delete network;
    delete builder;
    std::fprintf(stderr, "[trt] wrote %s\n", enginePath.c_str());
    return true;
}

bool Engine::introspect() {
    const int n = engine_->getNbIOTensors();
    for (int i = 0; i < n; ++i) {
        const char* name = engine_->getIOTensorName(i);
        const Dims d = engine_->getTensorShape(name);
        const bool isInput = engine_->getTensorIOMode(name) == TensorIOMode::kINPUT;

        if (isInput && d.nbDims == 4) {
            inName_   = name;
            batch_    = int(d.d[0]);
            channels_ = int(d.d[1]);
            imgsz_    = int(d.d[2]);
            if (d.d[2] != d.d[3])
                std::fprintf(stderr, "[trt] WARNING non-square input %ldx%ld\n",
                             long(d.d[2]), long(d.d[3]));
        } else if (!isInput && d.nbDims == 3) {
            outName_ = name;
            outC_    = int(d.d[1]);
            outA_    = int(d.d[2]);
        }
    }

    if (inName_.empty() || outName_.empty()) {
        std::fprintf(stderr, "[trt] could not identify input/output tensors\n");
        return false;
    }
    if (outC_ != 6) {
        std::fprintf(stderr,
            "[trt] output has %d channels, expected 6 (cx,cy,w,h,score,angle) for a "
            "single-class OBB head. The decode kernel assumes that layout -- stop and "
            "check the ONNX before trusting any detection.\n", outC_);
        return false;
    }

    const int expected = (imgsz_ / 8) * (imgsz_ / 8) + (imgsz_ / 16) * (imgsz_ / 16)
                       + (imgsz_ / 32) * (imgsz_ / 32);
    if (outA_ != expected)
        std::fprintf(stderr, "[trt] note: %d anchors, expected %d for imgsz %d\n",
                     outA_, expected, imgsz_);

    std::fprintf(stderr, "[trt] in '%s' %dx%dx%dx%d  out '%s' %dx%dx%d\n",
                 inName_.c_str(), batch_, channels_, imgsz_, imgsz_,
                 outName_.c_str(), batch_, outC_, outA_);
    return true;
}

bool Engine::createContexts(int n) {
    for (int i = 0; i < n; ++i) {
        IExecutionContext* c = engine_->createExecutionContext();
        if (!c) return false;
        ctx_.push_back(c);
    }
    return true;
}

bool Engine::enqueue(int i, void* dIn, void* dOut, cudaStream_t s) {
    IExecutionContext* c = ctx_[i];
    if (!c->setTensorAddress(inName_.c_str(), dIn))  return false;
    if (!c->setTensorAddress(outName_.c_str(), dOut)) return false;
    return c->enqueueV3(s);
}
