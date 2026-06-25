# NS3 integration with Linux TCP/IP Stack

This is a small network simulator project, trying to bypass NS3's inherent TCP/IP stack and add external libraries to NS3 builds to make the network simulator more rich and close to real world scenarios. Currently, this project will integrate the TCP/IP Stack of Linux with NS3.

This project won't build through normal CMake, we need to install NS3, and then inside /contrib we need to git pull this. Once, the project is done, I will include the Readme for building the binaries and the related dependencies.

Remove all OAuth external dependecies like Devin Desktop after things are done.

- Integrate QkdNetsim, to get Req from Linux Kernel, parse the JSON and use it to encrypt traffic. This has been demonstrated on small scale, we extend it to encryption, along with QKD Headers and Keys.
