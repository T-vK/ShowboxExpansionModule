#ifndef UNPROTECTEDMIDISTREAMOUT_H
#define UNPROTECTEDMIDISTREAMOUT_H

#include <MidiStreamOut.h>

namespace midi {

class UnprotectedMidiStreamOut : public MidiStreamOut {
public:
    using MidiStreamOut::MidiStreamOut;
    using MidiStreamOut::writeData;
};

} // namespace


#endif // UNPROTECTEDMIDISTREAMOUT_H