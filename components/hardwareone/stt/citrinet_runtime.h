#pragma once
// Citrinet build only (ENABLE_STT_MODEL_CITRINET): the runtime API itself is quartznet_runtime.h, shared by
// both models' runtimes.
#include "stt_lm_wordpiece.h"
#include <cstddef>
namespace hw1::stt {
// The WordPiece vocabulary compiled into the firmware (citrinet_vocab.h), parsed into PSRAM on the first
// transcribe() call and kept: the greedy fallback and the LM decoder (System_STTLocal.cpp) share it.
// nullptr until then; a decoder called from transcribe() always sees it set.
const lm::wordpiece::Pieces* citrinetPieces();
// The model's weights live in one PSRAM block for the whole boot (stt_model_slot.h): reserved here at boot,
// while PSRAM is still in one piece, or by the first transcribe() otherwise; never freed. Every session and
// one-shot call borrows it; only the first load reads the card. False (logged) when this board's PSRAM
// cannot spare it (then the first call tries again) or no single block that large is free.
bool reserveModel();
// The longest call transcribe() takes in one piece: once the image is trimmed to the resident model's
// weights (after the first session builds that model) only the resident model can run, and it is built for
// 10 s, so longer audio is transcribed in pieces of at most this many samples (sttLocalTranscribe splits
// it). 0 while calls of any length fit.
size_t singleCallSamples();
}
