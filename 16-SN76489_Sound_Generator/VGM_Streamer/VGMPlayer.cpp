#include "serialib.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fstream>
#include <vector>

// Modify these constants for your own setup
#define PORT "/dev/tty.usbserial-A50285BI"
#define BAUD 4800

// Input parser ripped from
// https://stackoverflow.com/questions/865668/parsing-command-line-arguments-in-c
class InputParser
{
public:
    InputParser(int &argc, char **argv)
    {
        for (int i = 1; i < argc; ++i)
            this->tokens.push_back(std::string(argv[i]));
    }
    /// @author iain
    const std::string &getCmdOption(const std::string &option) const
    {
        std::vector<std::string>::const_iterator itr;
        itr = std::find(this->tokens.begin(), this->tokens.end(), option);
        if (itr != this->tokens.end() && ++itr != this->tokens.end())
        {
            return *itr;
        }
        static const std::string empty_string("");
        return empty_string;
    }
    /// @author iain
    bool cmdOptionExists(const std::string &option) const
    {
        return std::find(this->tokens.begin(), this->tokens.end(), option) != this->tokens.end();
    }

private:
    std::vector<std::string> tokens;
};

struct t_vgmheader
{
    std::string ident = "VGM ";
    uint32_t eof_offset;
    uint32_t version;
    uint32_t sn76489_clock;
    uint32_t ym2413_clock;
    uint32_t gd3_offset;
    uint32_t total_samples;
    uint32_t loop_offset;
    uint32_t no_samples;
    uint32_t rate;
    uint16_t sn_fb;
    uint8_t  snw;
    uint8_t  sf;
    uint32_t ym2612_clock;
    uint32_t ym2151_clock;
    uint32_t vgm_data_offset;
    uint32_t segapcm_clock;
    uint32_t spcm_interface;
};

class vgmFile
{
public:
    vgmFile(std::string filename, bool v = false)
    {
        verbose = v;
        std::ifstream datafile(filename, std::ios::in | std::ios::binary);
        if (!datafile.is_open())
        {
            std::cout << "failed to open " << filename << '\n';
            exit(0);
        }
        else
        {
            std::vector<uint8_t> head(40);
            datafile.read((char *)&head[0], 40); // get header data (40 first bytes)

            header.ident = head[0]; //messy and probably not needed...
            header.ident += head[1];
            header.ident += head[2];
            header.ident += head[3];
            header.eof_offset = conv32(head, 0x4);
            header.version = conv32(head, 0x8);
            header.sn76489_clock = conv32(head, 0xC);
            header.ym2413_clock = conv32(head, 0x10);
            header.gd3_offset = conv32(head, 0x14);
            header.total_samples = conv32(head, 0x18);
            header.loop_offset = conv32(head, 0x1C);
            header.no_samples = conv32(head, 0x20);
            header.rate = conv32(head, 0x24);
            header.sn_fb = conv16(head, 0x28);
            header.snw = int(head[0x2A]);
            header.sf = head[0x2B];
            header.ym2612_clock = conv32(head, 0x2C);
            header.ym2151_clock = conv32(head, 0x30);
            header.vgm_data_offset = conv32(head, 0x34);
            header.segapcm_clock = conv32(head, 0x38);
            header.spcm_interface = conv32(head, 0x3C);

            if (verbose)
            {
                std::cout << "Header Information:\n"
                        << "Identifier:          " << header.ident << "\n"
                        << "EOF Offset:          " << std::dec << header.eof_offset << " Bytes\n"
                        << "Version:             0x" << std::hex << header.version << "\n"
                        << "SN76489 Clock:       " << std::dec << header.sn76489_clock << "Hz\n"
                        << "YM2413 Clock:        " << std::dec << header.ym2413_clock << "Hz\n"
                        << "GD3 Offset:          " << std::dec << header.gd3_offset << " Bytes\n"
                        << "Total Samples:       " << std::dec << header.total_samples << "\n"
                        << "Loop Offset:         " << std::dec << header.loop_offset << " Bytes\n"
                        << "Number of Samples:   " << std::dec << header.no_samples << "\n"
                        << "Rate:                " << std::dec << header.rate << "Hz\n"
                        << "SN76489 Feedback:    0x" << std::hex << header.sn_fb << "\n"
                        << "SN76489 Shift Width: " << std::dec << int(header.snw) << " Bits\n"
                        << "SN76489 Flags:       " << std::hex << int(header.sf) << "\n"
                        << "YM2612 Clock:        " << std::dec << header.ym2612_clock << "Hz\n"
                        << "YM2151 Clock:        " << std::dec << header.ym2151_clock << "Hz\n"
                        << "VGM Data Offset:     0x" << std::hex << header.vgm_data_offset << "\n"
                        << "Sega PCM Clock:      " << std::dec << header.segapcm_clock << "Hz\n"
                        << "Sega PCM Inteface:   " << std::dec << header.spcm_interface << "\n\n";
            }

            uint8_t n = 0;
            while (datafile.good())
            {
                datafile.read(reinterpret_cast<char *>(&n), sizeof n);
                data.push_back(n);
            }
            datafile.close();
            std::cout << filename << " loaded\n";
        }        
    };

    int openSerial(std::string port, u_int16_t baud)
    {
        // Connection to serial port
        char errorOpening = serial.openDevice(port.c_str(), baud);

        // If connection fails, return the error code otherwise, display a success message
        if (errorOpening != 1)
        {
            std::cout << "Failed to connect to " << port << ", error: " << int(errorOpening) << "\n";
            return errorOpening;
        }
        std::cout << "Successful connection to " << port << "\n";
        return errorOpening;
    }

    void closeSerial()
    {
        serial.closeDevice();
    }

    void sendIt()
    {
        uint32_t pos = 39; //data position
        float progress = 0.0; //progress indicator
        while (pos < data.size())
        {
            switch (data[pos])
            {
            case 0x50: //Send Next Byte to chip
                {
                    serial.writeChar(data[pos + 1]);
                    progress = pos / (float)data.size();

                    int barWidth = 70;

                    std::cout << "[";
                    int p = barWidth * progress;
                    for (int i = 0; i < barWidth; ++i)
                    {
                        if (i < p)
                            std::cout << "=";
                        else if (i == p)
                            std::cout << ">";
                        else
                            std::cout << " ";
                    }
                    std::cout << "] " << int(progress * 100.0) << " %\r";
                    std::cout.flush();
                    pos += 2;
                    break;
                }
            case 0x61: //Wait n samples, n = next two bytes in little endian format
                {
                    // Make 6F 25 into 256F
                    doDelay(conv16(data, pos+1));
                    pos += 3;
                    break;
                }
            case 0x62: //Wait 60th of a second
                {
                    doDelay(735); 
                    pos += 1;
                    break;
                }
            case 0x63: //Wait 50th of a second
                {
                    doDelay(882);
                    pos += 1;
                    break;
                }
            default: //If its not known, just skip
                pos += 1;
                break;
            }
        }
        serial.writeChar(0x66); // end of file
    }
    
    bool verbose = false;

private:
    t_vgmheader header;
    std::vector<uint8_t> data;
    serialib serial;

    u_int32_t conv32(std::vector<uint8_t> v, int i)
    {
        return (v[i+3] << 24) + (v[i+2] << 16) + (v[i+1] << 8) + v[i];
    }
    u_int16_t conv16(std::vector<uint8_t> v, int i)
    {
        return (v[i + 1] << 8) + v[i];
    }

    //delay routine for number of samples
    void doDelay(uint16_t samples)
    {
        for (uint16_t i = 0; i < samples; i++)
        {
            usleep(13); // 13 microseconds = 1 sample (on my computer, change if necessary)
        }
    }
};

int main(int argc, char *argv[])
{
    //defaults
    std::string filename = "";
    std::string port = PORT;
    u_int16_t baud = BAUD;
    
    //get command line
    InputParser input(argc, argv);
    if (input.cmdOptionExists("-h") || input.cmdOptionExists("-help"))
    {
        std::cout << "VGMPlayer - Send VGM file data via a serial port\n\n"
                  << "Usage : VGMPlayer [arguments]\n\nArguments:\n"
                  << "   -f       Filename (mandatory)\n"
                  << "   -p       Serial Port (default: " << PORT << ")\n"
                  << "   -b       Baud Rate (default: " << BAUD << ")\n"
                  << "   -v       Verbose (show header data)\n\n";
        return 0;
    }
    if (input.cmdOptionExists("-f"))
    {
        filename = input.getCmdOption("-f");
    }
    else
    {
        std::cout << "Must have a filename argument '-f <filename>'\n";
        return -1;
    }
    if (input.cmdOptionExists("-p"))
    {
        port = input.getCmdOption("-p");
    }
    if (input.cmdOptionExists("-b"))
    {
        baud = std::stoi(input.getCmdOption("-b"));
    }

    //Load VGM data
    vgmFile vgm(filename, input.cmdOptionExists("-v"));

    std::cout << "Connecting to port " << port << " at baud rate of " << int(baud) << "\n";
    if (vgm.openSerial(port, baud) != 1)
    {
        return -1;
    }

    //Send Tone data
    vgm.sendIt();
    //Close Serial Port
    vgm.closeSerial();
    return 0;
}
