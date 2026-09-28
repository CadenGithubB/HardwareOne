#include "System_BuildConfig.h"
#include "System_STTLocal.h"
#if ENABLE_LOCAL_STT
#include <cstdio>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "System_VFS.h"
#include "stt/quartznet_runtime.h"
#include "stt/stt_model_identity.h"
namespace {
constexpr const char* kModelPath="/STT Models/quartznet5x5.p4.stt";
File openModel() {
    // trusted: fixed, pinned inference weights; no caller-selected file path.
    return VFS::openGuarded(kModelPath,"r",VFS::systemAuth("stt.model_read"));
}
size_t readModel(void* context,void* dest,size_t bytes) {
    return static_cast<File*>(context)->read(static_cast<uint8_t*>(dest),bytes);
}
}
bool sttLocalAvailable(char* error,size_t cap) {
    if(error&&cap)error[0]=0;
    File file=openModel();
    const bool ok=file&&!file.isDirectory()&&file.size()==96+hw1::stt::identity::kCompressedBytes;
    if(!ok&&error&&cap)snprintf(error,cap,"Install the matching local STT model in /STT Models");
    return ok;
}
bool sttLocalTranscribe(const int16_t* pcm,size_t samples,char* text,size_t textCap,
                       const STTLocalControl& control,STTLocalStats& stats,
                       char* error,size_t errorCap) {
    File file=openModel();
    if(!file||file.isDirectory()) {
        if(text&&textCap)text[0]=0;
        if(error&&errorCap)snprintf(error,errorCap,"Cannot open local STT model");
        return false;
    }
    return hw1::stt::transcribe({&file,file.size(),readModel},pcm,samples,text,textCap,control,stats,error,errorCap);
}
#else
bool sttLocalAvailable(char* error,size_t cap) {
    if(error&&cap)snprintf(error,cap,"No local STT backend is qualified for this target");
    return false;
}
bool sttLocalTranscribe(const int16_t*,size_t,char* text,size_t cap,const STTLocalControl&,
                       STTLocalStats& stats,char* error,size_t errorCap) {
    if(text&&cap)text[0]=0;stats={};return sttLocalAvailable(error,errorCap);
}
#endif
#endif
