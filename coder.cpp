#include "coder.h"

#include <stdio.h>
#include <stdlib.h>

#include <QDebug>
#include <string>

#ifdef _WIN32
#define _CRT_SECURE_NO_DEPRECATE 1
#endif

#ifdef __cplusplus
extern "C" {
#include "SKP_Silk_SDK_API.h"
#include "SKP_Silk_SigProc_FIX.h"
#include "mp3encoder.h"
}
#endif

#ifdef _SYSTEM_IS_BIG_ENDIAN
/* Function to convert a little endian int16 to a */
/* big endian int16 or vica verca                 */
void swap_endian(SKP_int16 vec[], SKP_int len) {
  SKP_int i;
  SKP_int16 tmp;
  SKP_uint8 *p1, *p2;

  for (i = 0; i < len; i++) {
    tmp = vec[i];
    p1 = (SKP_uint8 *)&vec[i];
    p2 = (SKP_uint8 *)&tmp;
    p1[0] = p2[1];
    p1[1] = p2[0];
  }
}
#endif

#if (defined(_WIN32) || defined(_WINCE))
#include <windows.h> /* timer */
#else                // Linux or Mac
#include <sys/time.h>
#endif

#ifdef _WIN32

unsigned long GetHighResolutionTime() /* O: time in usec*/
{
  /* Returns a time counter in microsec	*/
  /* the resolution is platform dependent */
  /* but is typically 1.62 us resolution  */
  LARGE_INTEGER lpPerformanceCount;
  LARGE_INTEGER lpFrequency;
  QueryPerformanceCounter(&lpPerformanceCount);
  QueryPerformanceFrequency(&lpFrequency);
  return (unsigned long)((1000000 * (lpPerformanceCount.QuadPart)) /
                         lpFrequency.QuadPart);
}
#else   // Linux or Mac
unsigned long GetHighResolutionTime() /* O: time in usec*/
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return ((tv.tv_sec * 1000000) + (tv.tv_usec));
}
#endif  // _WIN32

/* Seed for the random number generator, which is used for simulating packet
 * loss */
static SKP_int32 rand_seed = 1;

Coder::Coder(std::string inputPath, std::string outputPath) {
  this->inputPath = inputPath;
  this->outputPath = outputPath;
}

std::string Coder::encode() { return ""; }

std::string Coder::decode() {
  unsigned long starttime;
  double filetime;
  size_t counter;
  SKP_int32 totPackets, i, k;
  SKP_int16 ret, len, tot_len;
  SKP_int16 nBytes;
  SKP_uint8
      payload[MAX_BYTES_PER_FRAME * MAX_INPUT_FRAMES * (MAX_LBRR_DELAY + 1)];
  SKP_uint8 *payloadEnd = NULL, *payloadToDec = NULL;
  SKP_uint8 FECpayload[MAX_BYTES_PER_FRAME * MAX_INPUT_FRAMES], *payloadPtr;
  SKP_int16 nBytesFEC;
  SKP_int16 nBytesPerPacket[MAX_LBRR_DELAY + 1], totBytes;
  SKP_int16 out[((FRAME_LENGTH_MS * MAX_API_FS_KHZ) << 1) * MAX_INPUT_FRAMES],
      *outPtr;
  char speechOutFileName[150], bitInFileName[150];
  FILE *bitInFile, *speechOutFile;
  SKP_int32 packetSize_ms = 0, API_Fs_Hz = 0;
  SKP_int32 decSizeBytes;
  void *psDec;
  SKP_float loss_prob;
  SKP_int32 frames, lost, quiet;
  SKP_SILK_SDK_DecControlStruct DecControl;

  /* check if path is null */
  if (inputPath.empty() || outputPath.empty()) {
    return "input or outpath is null";
  }
  /* default settings */
  quiet = 1;
  loss_prob = 0.0f;

  strcpy(bitInFileName, inputPath.data());
  strcpy(speechOutFileName, outputPath.data());
  qDebug() << "input: " << bitInFileName;
  qDebug() << "output: " << speechOutFileName;

  /* Open files */
  bitInFile = fopen(bitInFileName, "rb");
  if (bitInFile == NULL) {
    return "Error: could not open input file.";
  }

  speechOutFile = fopen(speechOutFileName, "wb");
  if (speechOutFile == NULL) {
    return "Error: could not open output file.";
  }

  /* Check Silk header */
  {
    char header_buf[50];
    fread(header_buf, sizeof(char), 1, bitInFile);
    header_buf[strlen("")] = '\0'; /* Terminate with a null character */
    if (strcmp(header_buf, "") != 0) {
      counter = fread(header_buf, sizeof(char), strlen("!SILK_V3"), bitInFile);
      header_buf[strlen("!SILK_V3")] =
          '\0'; /* Terminate with a null character */
      if (strcmp(header_buf, "!SILK_V3") != 0) {
        /* Non-equal strings */
        return "Error: Wrong Header.";
      }
    } else {
      counter = fread(header_buf, sizeof(char), strlen("#!SILK_V3"), bitInFile);
      header_buf[strlen("#!SILK_V3")] =
          '\0'; /* Terminate with a null character */
      if (strcmp(header_buf, "#!SILK_V3") != 0) {
        /* Non-equal strings */
        return "Error: Wrong Header.";
      }
    }
  }

  /* Set the samplingrate that is requested for the output */
  if (API_Fs_Hz == 0) {
    DecControl.API_sampleRate = 24000;
  } else {
    DecControl.API_sampleRate = API_Fs_Hz;
  }

  /* Initialize to one frame per packet, for proper concealment before first
   * packet arrives */
  DecControl.framesPerPacket = 1;

  /* Create decoder */
  ret = SKP_Silk_SDK_Get_Decoder_Size(&decSizeBytes);
  if (ret) {
    qDebug() << "SKP_Silk_SDK_Get_Decoder_Size returned " << ret;
  }
  psDec = malloc(decSizeBytes);

  /* Reset decoder */
  ret = SKP_Silk_SDK_InitDecoder(psDec);
  if (ret) {
    qDebug() << "SKP_Silk_InitDecoder returned " << ret;
  }

  totPackets = 0;
  int tottime = 0;
  payloadEnd = payload;
  /* Simulate the jitter buffer holding MAX_FEC_DELAY packets */
  for (i = 0; i < MAX_LBRR_DELAY; i++) {
    /* Read payload size */
    counter = fread(&nBytes, sizeof(SKP_int16), 1, bitInFile);
#ifdef _SYSTEM_IS_BIG_ENDIAN
    swap_endian(&nBytes, 1);
#endif
    /* Read payload */
    counter = fread(payloadEnd, sizeof(SKP_uint8), nBytes, bitInFile);

    if ((SKP_int16)counter < nBytes) {
      break;
    }
    nBytesPerPacket[i] = nBytes;
    payloadEnd += nBytes;
    totPackets++;
  }

  while (1) {
    /* Read payload size */
    counter = fread(&nBytes, sizeof(SKP_int16), 1, bitInFile);
#ifdef _SYSTEM_IS_BIG_ENDIAN
    swap_endian(&nBytes, 1);
#endif
    if (nBytes < 0 || counter < 1) {
      break;
    }

    /* Read payload */
    counter = fread(payloadEnd, sizeof(SKP_uint8), nBytes, bitInFile);
    if ((SKP_int16)counter < nBytes) {
      break;
    }

    /* Simulate losses */
    rand_seed = SKP_RAND(rand_seed);
    if ((((float)((rand_seed >> 16) + (1 << 15))) / 65535.0f >=
         (loss_prob / 100.0f)) &&
        (counter > 0)) {
      nBytesPerPacket[MAX_LBRR_DELAY] = nBytes;
      payloadEnd += nBytes;
    } else {
      nBytesPerPacket[MAX_LBRR_DELAY] = 0;
    }

    if (nBytesPerPacket[0] == 0) {
      /* Indicate lost packet */
      lost = 1;

      /* Packet loss. Search after FEC in next packets. Should be done in the
       * jitter buffer */
      payloadPtr = payload;
      for (i = 0; i < MAX_LBRR_DELAY; i++) {
        if (nBytesPerPacket[i + 1] > 0) {
          starttime = GetHighResolutionTime();
          SKP_Silk_SDK_search_for_LBRR(payloadPtr, nBytesPerPacket[i + 1],
                                       (i + 1), FECpayload, &nBytesFEC);
          tottime += GetHighResolutionTime() - starttime;
          if (nBytesFEC > 0) {
            payloadToDec = FECpayload;
            nBytes = nBytesFEC;
            lost = 0;
            break;
          }
        }
        payloadPtr += nBytesPerPacket[i + 1];
      }
    } else {
      lost = 0;
      nBytes = nBytesPerPacket[0];
      payloadToDec = payload;
    }

    /* Silk decoder */
    outPtr = out;
    tot_len = 0;
    starttime = GetHighResolutionTime();

    if (lost == 0) {
      /* No Loss: Decode all frames in the packet */
      frames = 0;
      do {
        /* Decode 20 ms */
        ret = SKP_Silk_SDK_Decode(psDec, &DecControl, 0, payloadToDec, nBytes,
                                  outPtr, &len);
        if (ret) {
          printf("\nSKP_Silk_SDK_Decode returned %d", ret);
        }

        frames++;
        outPtr += len;
        tot_len += len;
        if (frames > MAX_INPUT_FRAMES) {
          /* Hack for corrupt stream that could generate too many frames */
          outPtr = out;
          tot_len = 0;
          frames = 0;
        }
        /* Until last 20 ms frame of packet has been decoded */
      } while (DecControl.moreInternalDecoderFrames);
    } else {
      /* Loss: Decode enough frames to cover one packet duration */
      for (i = 0; i < DecControl.framesPerPacket; i++) {
        /* Generate 20 ms */
        ret = SKP_Silk_SDK_Decode(psDec, &DecControl, 1, payloadToDec, nBytes,
                                  outPtr, &len);
        if (ret) {
          printf("\nSKP_Silk_Decode returned %d", ret);
        }
        outPtr += len;
        tot_len += len;
      }
    }

    packetSize_ms = tot_len / (DecControl.API_sampleRate / 1000);
    tottime += GetHighResolutionTime() - starttime;
    totPackets++;

    /* Write output to file */
#ifdef _SYSTEM_IS_BIG_ENDIAN
    swap_endian(out, tot_len);
#endif
    fwrite(out, sizeof(SKP_int16), tot_len, speechOutFile);

    /* Update buffer */
    totBytes = 0;
    for (i = 0; i < MAX_LBRR_DELAY; i++) {
      totBytes += nBytesPerPacket[i + 1];
    }
    /* Check if the received totBytes is valid */
    if (totBytes < 0 || totBytes > sizeof(payload)) {
      return "Packets decoded failed.";
    }
    SKP_memmove(payload, &payload[nBytesPerPacket[0]],
                totBytes * sizeof(SKP_uint8));
    payloadEnd -= nBytesPerPacket[0];
    SKP_memmove(nBytesPerPacket, &nBytesPerPacket[1],
                MAX_LBRR_DELAY * sizeof(SKP_int16));

    if (!quiet) {
      fprintf(stderr, "\rPackets decoded:             %d", totPackets);
    }
  }

  /* Empty the recieve buffer */
  for (k = 0; k < MAX_LBRR_DELAY; k++) {
    if (nBytesPerPacket[0] == 0) {
      /* Indicate lost packet */
      lost = 1;
      /* Packet loss. Search after FEC in next packets. Should be done in the
       * jitter buffer */
      payloadPtr = payload;
      for (i = 0; i < MAX_LBRR_DELAY; i++) {
        if (nBytesPerPacket[i + 1] > 0) {
          starttime = GetHighResolutionTime();
          SKP_Silk_SDK_search_for_LBRR(payloadPtr, nBytesPerPacket[i + 1],
                                       (i + 1), FECpayload, &nBytesFEC);
          tottime += GetHighResolutionTime() - starttime;
          if (nBytesFEC > 0) {
            payloadToDec = FECpayload;
            nBytes = nBytesFEC;
            lost = 0;
            break;
          }
        }
        payloadPtr += nBytesPerPacket[i + 1];
      }
    } else {
      lost = 0;
      nBytes = nBytesPerPacket[0];
      payloadToDec = payload;
    }

    /* Silk decoder */
    outPtr = out;
    tot_len = 0;
    starttime = GetHighResolutionTime();

    if (lost == 0) {
      /* No loss: Decode all frames in the packet */
      frames = 0;
      do {
        /* Decode 20 ms */
        ret = SKP_Silk_SDK_Decode(psDec, &DecControl, 0, payloadToDec, nBytes,
                                  outPtr, &len);
        if (ret) {
          printf("\nSKP_Silk_SDK_Decode returned %d", ret);
        }

        frames++;
        outPtr += len;
        tot_len += len;
        if (frames > MAX_INPUT_FRAMES) {
          /* Hack for corrupt stream that could generate too many frames */
          outPtr = out;
          tot_len = 0;
          frames = 0;
        }
        /* Until last 20 ms frame of packet has been decoded */
      } while (DecControl.moreInternalDecoderFrames);
    } else {
      /* Loss: Decode enough frames to cover one packet duration */

      /* Generate 20 ms */
      for (i = 0; i < DecControl.framesPerPacket; i++) {
        ret = SKP_Silk_SDK_Decode(psDec, &DecControl, 1, payloadToDec, nBytes,
                                  outPtr, &len);
        if (ret) {
          printf("\nSKP_Silk_Decode returned %d", ret);
        }
        outPtr += len;
        tot_len += len;
      }
    }
    packetSize_ms = tot_len / (DecControl.API_sampleRate / 1000);
    tottime += GetHighResolutionTime() - starttime;
    totPackets++;

    /* Write output to file */
#ifdef _SYSTEM_IS_BIG_ENDIAN
    swap_endian(out, tot_len);
#endif
    fwrite(out, sizeof(SKP_int16), tot_len, speechOutFile);

    /* Update Buffer */
    totBytes = 0;
    for (i = 0; i < MAX_LBRR_DELAY; i++) {
      totBytes += nBytesPerPacket[i + 1];
    }

    /* Check if the received totBytes is valid */
    if (totBytes < 0 || totBytes > sizeof(payload)) {
      return "Packets decoded failed.";
    }

    SKP_memmove(payload, &payload[nBytesPerPacket[0]],
                totBytes * sizeof(SKP_uint8));
    payloadEnd -= nBytesPerPacket[0];
    SKP_memmove(nBytesPerPacket, &nBytesPerPacket[1],
                MAX_LBRR_DELAY * sizeof(SKP_int16));

    if (!quiet) {
      fprintf(stderr, "\rPackets decoded:              %d", totPackets);
    }
  }

  /* Free decoder */
  free(psDec);

  /* Close files */
  fclose(speechOutFile);
  fclose(bitInFile);

  filetime = totPackets * 1e-3 * packetSize_ms;
  if (!quiet) {
    qDebug() << "File length: " << filetime << "s";
    // printf("Time for decoding:           %.3f s (%.3f%% of realtime)", 1e-6 *
    // tottime, 1e-4 * tottime / filetime);
  }

  Mp3Encoder *mp3encoder = new Mp3Encoder;
  int sampleRate = QQ_sampleRate;
  int channels = 2;
  int bitRate = 196;
  // 初始化解码器，传入源文件路径，生成的文件路径，采样频率，声道数，码率
  mp3encoder->Init(
      outputPath.c_str(),
      (outputPath.substr(0, outputPath.length() - 3) + "mp3").c_str(),
      sampleRate, channels, bitRate);
  // 编码
  mp3encoder->Encode();
  //关闭文件
  mp3encoder->Destory();

  delete mp3encoder;

  return "0";
}
