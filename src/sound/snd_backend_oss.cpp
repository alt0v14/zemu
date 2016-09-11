#if !defined(_WIN32) && !defined(__APPLE__)

#include "../defines.h"
#include "snd_backend_oss.h"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/soundcard.h>
#include <cmath>

CSndBackendOSS::CSndBackendOSS(unsigned frag_num, unsigned frag_size) {
  frag = (frag_num << 16) | (int)ceil(std::log2(frag_size));
}

void CSndBackendOSS::Init() {
  int tmp;

  audio = open("/dev/dsp", O_WRONLY, 0);
  if (audio == -1) StrikeError("Unable to open /dev/dsp for writing.");

  if (ioctl(audio, SNDCTL_DSP_SETFRAGMENT, &frag) == -1) {
    close(audio);
    StrikeError("Unable to set audio fragment size.");
  }

  tmp = AFMT_S16_NE;
  if (ioctl(audio, SNDCTL_DSP_SETFMT, &tmp) == -1) {
    close(audio);
    StrikeError("setting SNDCTL_DSP_SETFMT on audiodev failed.");
  }

  tmp = 16;
  ioctl(audio, SNDCTL_DSP_SAMPLESIZE, &tmp);
  if (tmp != 16) StrikeError("Unable set samplesize = %d.", tmp);

  tmp = 1;
  if (ioctl(audio, SNDCTL_DSP_STEREO, &tmp) == -1) {
    close(audio);
    StrikeError("Unable to set stereo.");
  }

  tmp = SND_FQ;
  if (ioctl(audio, SNDCTL_DSP_SPEED, &tmp) == -1) {
    close(audio);
    StrikeError("Unable to set audio speed = %d.", SND_FQ);
  }
}

void CSndBackendOSS::Write(uint8_t *buf, unsigned spbsize) {
  if (write(audio, buf, spbsize) != (int)spbsize) printf("Write to soundcard device failed\n");
}

CSndBackendOSS::~CSndBackendOSS() {
  close(audio);
}

#endif // !_WIN32
