Downloading the EiBi schedule no longer produces station names made of leftover stack bytes. A line whose name column was shorter than the fixed width left the name buffer untouched, so whatever the previous call had put on the stack was validated, accented-character mapped and finally displayed as the station name. The name is now read from the actual remainder of the line, and lines without a name are dropped instead of being stored with an empty one.

The schedule lookup also starts over when the frequency changes. The cached file offsets belong to the previous frequency, so seeking and the remote command could show the name of a station that shares the channel with the one just tuned.

