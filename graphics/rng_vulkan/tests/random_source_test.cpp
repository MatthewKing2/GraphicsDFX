#include "random_source.hpp"
#include "reference.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::uint32_t count=3, blocks=2, seed=42, workload=1;
constexpr std::size_t payloadBytes=count*blocks*16;

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
void appendWord(Bytes& bytes, std::uint32_t word) {
    for(unsigned byte=0; byte<4; ++byte) bytes.push_back(static_cast<unsigned char>(word >> (byte*8)));
}
Bytes frame(std::uint32_t epoch) {
    Bytes bytes;
    for(auto value: {rnglab::randomStreamMagic,rnglab::randomStreamVersion,count,blocks,epoch,seed,workload,0u})
        appendWord(bytes,value);
    for(std::uint32_t block=0; block<blocks; ++block)
        for(std::uint32_t id=0; id<count; ++id)
            for(auto value:rnglab::randomBlock(id,block,epoch,workload,seed)) appendWord(bytes,value);
    return bytes;
}
void checkPayload(const std::vector<rnglab::Words>& payload, std::uint32_t epoch) {
    for(std::uint32_t block=0; block<blocks; ++block)
        for(std::uint32_t id=0; id<count; ++id)
            require(payload[block*count+id]==rnglab::randomBlock(id,block,epoch,workload,seed),
                    "external payload differs from CPU Philox reference");
}
void read(rnglab::RandomStream& source, std::vector<rnglab::Words>& payload, std::uint32_t epoch) {
    source.readFrame(payload.data(),payloadBytes,count,blocks,epoch,seed,workload);
}
void expectFailure(const std::function<void()>& action, const std::string& text) {
    try {action();} catch(const std::exception& error) {
        if(std::string(error.what()).find(text)!=std::string::npos) return;
        throw std::runtime_error("Wrong error; expected '"+text+"', received '"+error.what()+"'");
    }
    throw std::runtime_error("Missing expected error: "+text);
}
struct Fixtures {
    std::filesystem::path root;
    unsigned serial=0;
    Fixtures() {
        root=std::filesystem::temp_directory_path()/(
            "rng-stream-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if(!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot create fixture directory");
    }
    ~Fixtures() {std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    std::string save(const Bytes& bytes) {
        auto path=(root/("fixture-"+std::to_string(serial++)+".bin")).string();
        std::ofstream file(path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!file) throw std::runtime_error("Cannot write fixture");
        return path;
    }
};

#ifndef _WIN32
void fifoShortReads(Fixtures& fixtures) {
    const auto path=(fixtures.root/"chunked.fifo").string();
    if(::mkfifo(path.c_str(),0600)!=0) throw std::runtime_error("Cannot create test FIFO");
    const auto bytes=frame(0);
    const pid_t writer=::fork();
    if(writer<0) throw std::runtime_error("Cannot fork test FIFO writer");
    if(writer==0) {
        const int fd=::open(path.c_str(),O_WRONLY);
        if(fd<0) ::_exit(2);
        for(std::size_t sent=0; sent<bytes.size();) {
            const auto amount=::write(fd,bytes.data()+sent,std::min(std::size_t(3),bytes.size()-sent));
            if(amount<0) {if(errno==EINTR) continue; ::_exit(3);}
            sent+=static_cast<std::size_t>(amount);
            ::usleep(1000); // Force the reader to assemble a frame from short reads.
        }
        ::close(fd);
        ::_exit(0);
    }
    try {
        rnglab::RandomStream source(path);
        std::vector<rnglab::Words> payload(count*blocks);
        read(source,payload,0);
        checkPayload(payload,0);
        int status=0;
        while(::waitpid(writer,&status,0)<0) {if(errno!=EINTR) throw std::runtime_error("waitpid failed");}
        require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"FIFO writer failed");
    } catch(...) {
        ::kill(writer,SIGTERM);
        while(::waitpid(writer,nullptr,0)<0&&errno==EINTR) {}
        throw;
    }
}
#endif
}

int main() {
    try {
        Fixtures fixtures;
        std::vector<rnglab::Words> payload(count*blocks);
        auto twoFrames=frame(0);
        const auto second=frame(1);
        twoFrames.insert(twoFrames.end(),second.begin(),second.end());
        {
            rnglab::RandomStream source(fixtures.save(twoFrames));
            read(source,payload,0);checkPayload(payload,0);
            read(source,payload,1);checkPayload(payload,1);
            expectFailure([&]{read(source,payload,2);},"frame header");
        }
        // All metadata fields must match before any payload bytes are accepted.
        const char* names[]{"magic","version","count","blocks","epoch","seed","workload","flags"};
        for(unsigned field=0; field<8; ++field) {
            auto bad=frame(0);bad[field*4]^=0x40;
            rnglab::RandomStream source(fixtures.save(bad));
            expectFailure([&]{read(source,payload,0);},std::string(names[field])+" mismatch");
        }
        {
            auto shortHeader=frame(0);shortHeader.resize(19);
            rnglab::RandomStream source(fixtures.save(shortHeader));
            expectFailure([&]{read(source,payload,0);},"frame header");
        }
        {
            auto shortPayload=frame(0);shortPayload.pop_back();
            rnglab::RandomStream source(fixtures.save(shortPayload));
            expectFailure([&]{read(source,payload,0);},"frame payload");
        }
        {
            rnglab::RandomStream source(fixtures.save(frame(0)));
            expectFailure([&]{source.readFrame(payload.data(),payloadBytes-1,count,blocks,0,seed,workload);},"byte count");
            expectFailure([&]{source.readFrame(nullptr,payloadBytes,count,blocks,0,seed,workload);},"destination");
            read(source,payload,0);checkPayload(payload,0); // Invalid calls did not consume the stream.
        }
        expectFailure([&]{rnglab::RandomStream source((fixtures.root/"missing").string());},"Cannot open");
#ifndef _WIN32
        fifoShortReads(fixtures);
#endif
        std::cout << "PASS RNG1 frames, metadata, truncation, layout and chunked FIFO reads\n";
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
