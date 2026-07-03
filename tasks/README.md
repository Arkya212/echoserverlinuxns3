# NS3 integration with Linux TCP/IP Stack

This is a small network simulator project, trying to bypass NS3's inherent TCP/IP stack and add external libraries to NS3 builds to make the network simulator more rich and close to real world scenarios. Currently, this project will integrate the TCP/IP Stack of Linux with NS3.

This project won't build through normal CMake, we need to install NS3, and then inside /contrib we need to git pull this. Once, the project is done, I will include the Readme for building the binaries and the related dependencies.

Remove all OAuth external dependecies like Devin Desktop after things are done.

- Integrate QkdNetsim, to get Req from Linux Kernel, parse the JSON and use it to encrypt traffic. This has been demonstrated on small scale, we extend it to encryption, along with QKD Headers and Keys.

## Building a HTTP Server
Benchmarking HTTP Servers: https://github.com/wg/wrk

Socket Programming Guide:
We can use this to read about Socket Programming and Start impl small servers from IBM Doc:
https://www.cs.cornell.edu/~kvikram/HTMLS/MLA/NET.PDF
https://www.ibm.com/docs/en/SSLTBW_2.3.0/pdf/hala001_v2r3.pdf#page=126&zoom=100,0,80

These teach about how HTTP is split into diff architectures:
https://sookocheff.com/post/api/marrying-restful-http-with-asynchronous-design/

This tells about diff server architectures:
https://berb.github.io/diploma-thesis/original/042_serverarch.html

These give the High Level Design Patterns for Cloud Architectures:
https://learn.microsoft.com/en-us/azure/architecture/patterns/asynchronous-request-reply

READ THIS As the final Server with all the features and Benchmarks

These 2 have the necessary good start along with lot of scope to expand along with LLD:
Also, they build from TCP layer, so good enough:

https://trungams.github.io/2020-08-23-a-simple-http-server-from-scratch/
https://medium.com/@himalee.tailor/designing-a-http-server-96382d8a0507


