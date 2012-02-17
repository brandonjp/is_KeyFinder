/*************************************************************************

  Copyright 2011 Ibrahim Sha'ath

  This file is part of KeyFinder.

  KeyFinder is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  KeyFinder is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with KeyFinder.  If not, see <http://www.gnu.org/licenses/>.

*************************************************************************/

#include "decoderlibav.h"

AudioStream* LibAvDecoder::decodeFile(const QString& filePath){

  AVCodec* codec = NULL;
  AVFormatContext* fCtx = NULL;
  AVCodecContext* cCtx = NULL;
  AVDictionary* dict = NULL;

  // convert filepath
#ifdef Q_OS_WIN
  const wchar_t* filePathWc = reinterpret_cast<const wchar_t*>(filePath.constData());
  const char* filePathCh = utf16_to_utf8(filePathWc);
#else
  QByteArray encodedPath = QFile::encodeName(filePath);
  const char* filePathCh = encodedPath;
#endif

  // open file
  if(avformat_open_input(&fCtx, filePathCh, NULL, NULL) != 0){
    qCritical("Failed to open audio file: %s", filePathCh);
    throw Exception();
  }
  if(av_find_stream_info(fCtx) < 0){
    qCritical("Failed to find stream information in file: %s", filePathCh);
    throw Exception();
  }
  int audioStream = -1;
  for(int i=0; i<(signed)fCtx->nb_streams; i++){
    if(fCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO){
      audioStream = i;
      break;
    }
  }
  if(audioStream == -1){
    qCritical("Failed to find an audio stream in file: %s", filePathCh);
    throw Exception();
  }
  // Determine stream codec
  cCtx = fCtx->streams[audioStream]->codec;
  codec = avcodec_find_decoder(cCtx->codec_id);
  if(codec == NULL){
    qCritical("Audio stream has unsupported codec in file: %s", filePathCh);
    throw Exception();
  }
  // Open codec
  int codecOpenResult = avcodec_open2(cCtx, codec, &dict);
  if(codecOpenResult < 0){
    qCritical("Error opening audio codec: %s (%d)", codec->long_name, codecOpenResult);
    throw Exception();
  }

  // Prep buffer
  AudioStream *astrm = new AudioStream();
  astrm->setFrameRate(cCtx->sample_rate);
  astrm->setChannels(cCtx->channels);
  // Decode stream
  av_init_packet(&avpkt);
  int badPacketCount = 0;
  while(av_read_frame(fCtx, &avpkt) == 0){
    if(avpkt.stream_index == audioStream){
      try{
        int result = decodePacket(cCtx, &avpkt, astrm);
        if(result != 0){
          if(badPacketCount < 100){
            badPacketCount++;
          }else{
            qCritical("100 bad packets, may be DRM or corruption in file: %s", filePathCh);
            throw Exception();
          }
        }
      }catch(Exception& e){
        throw e;
      }
    }
    av_free_packet(&avpkt);
  }

  int codecCloseResult = avcodec_close(cCtx);
  if(codecCloseResult < 0){
    qCritical("Error closing audio codec: %s (%d)", codec->long_name, codecCloseResult);
  }

  av_close_input_file(fCtx);
  return astrm;
}

int LibAvDecoder::decodePacket(AVCodecContext* cCtx, AVPacket* avpkt, AudioStream* ab){
  while(avpkt->size > 0){
    int outputBufferSize = ((AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2) * sizeof(int16_t);
    int16_t* outputBuffer = (int16_t*)av_malloc(outputBufferSize);
    int bytesConsumed = avcodec_decode_audio3(cCtx, outputBuffer, &outputBufferSize, avpkt);
    if(bytesConsumed <= 0){
      avpkt->size = 0;
      av_free(outputBuffer);
      return 1;
    }
    int newSamplesDecoded = outputBufferSize / sizeof(int16_t);
    int oldSampleCount = ab->getSampleCount();
    try{
      ab->addToSampleCount(newSamplesDecoded);
    }catch(Exception& e){
      av_free(outputBuffer);
      throw e;
    }
    for(int i = 0; i < newSamplesDecoded; i++)
      ab->setSample(oldSampleCount+i, (float)outputBuffer[i]);
    if(bytesConsumed < avpkt->size){
      size_t newLength = avpkt->size - bytesConsumed;
      uint8_t* datacopy = avpkt->data;
      avpkt->data = (uint8_t*)av_malloc(newLength);
      memcpy(avpkt->data, datacopy + bytesConsumed, newLength);
      av_free(datacopy);
    }
    avpkt->size -= bytesConsumed;
    av_free(outputBuffer);
  }
  return 0;
}

// Thread safety is a bit more complex here, see av_lockmgr_register documentation
int libAvMutexManager(void** av_mutex, enum AVLockOp op){
  QMutex* libAvMutex;
  switch(op){
  case AV_LOCK_CREATE:
    try{
      libAvMutex = new QMutex();
      *av_mutex = libAvMutex;
    }catch(...){
      return 1;
    }
    return 0;
  case AV_LOCK_OBTAIN:
    libAvMutex = (QMutex*)*av_mutex;
    if(libAvMutex->tryLock()){
      return 0;
    }else{
      return 1;
    }
  case AV_LOCK_RELEASE:
    libAvMutex = (QMutex*)*av_mutex;
    libAvMutex->unlock();
    return 0;
  case AV_LOCK_DESTROY:
    libAvMutex = (QMutex*)*av_mutex;
    delete libAvMutex;
    *av_mutex = NULL;
    return 0;
  }
  return 1;
}
