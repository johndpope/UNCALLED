#include <thread>
#include <chrono>
#include "fast5_pool.hpp"
#include "mapper.hpp"


bool open_fast5(const std::string &filename, fast5::File &file) {
    if (!fast5::File::is_valid_file(filename)) {
        std::cerr << "Error: '" << filename << "' is not a valid file \n";
    }

    try {
        file.open(filename);
        
        if (!file.is_open()) {  
            std::cerr << "Error: unable to open '" << filename << "'\n";
            return false;
        }

        return true;
        
    } catch (hdf5_tools::Exception& e) {
        std::cerr << "Error: hdf5 exception '" << e.what() << "'\n";
        return false;
    }

    return false;
}


Fast5Pool::Fast5Pool(MapperParams &params, u16 nthreads, u32 batch_size) {
    nthreads_ = nthreads;
    batch_size_ = batch_size;

    for (u16 i = 0; i < nthreads_; i++) {
        //threads_.push_back(MapperThread(params));
        threads_.emplace_back(params);
    }
    for (MapperThread &t : threads_) {
        t.start();
    }
}

void Fast5Pool::add_fast5s(const std::list<std::string> &new_fast5s) {
    fast5s_.insert(fast5s_.end(), new_fast5s.begin(), new_fast5s.end());
}

std::vector<std::string> Fast5Pool::update() {
    std::vector<std::string> ret;

    for (MapperThread &t : threads_) {
        t.out_mtx_.lock();
        while (!t.locs_out_.empty()) {
            //TODO: parse outside
            ret.push_back(t.locs_out_.front().str());
            t.locs_out_.pop_front();
        }
        t.out_mtx_.unlock();

        if (t.signals_in_.size() < batch_size_ / 2) {
            while (!fast5s_.empty() && t.signals_in_.size() < batch_size_) {

                std::string fname = fast5s_.front();
                fast5s_.pop_front();

                fast5::File fast5;
                open_fast5(fname, fast5);            

                std::vector<float> samples = fast5.get_raw_samples();
                std::string ids = fast5.get_raw_samples_params().read_id;

                //TODO: buffer signal
                t.in_mtx_.lock();
                t.signals_in_.push_back(samples);
                t.ids_in_.push_back(ids);
                fast5.close();
                t.in_mtx_.unlock();
            }
        }
    }
    
    return ret;
}

bool Fast5Pool::all_finished() {
    if (!fast5s_.empty()) return false;

    for (MapperThread &t : threads_) {
        if (t.aligning_ || !t.locs_out_.empty()) return false;
    }

    return true;
}

void Fast5Pool::stop_all() {
    for (MapperThread &t : threads_) {
        t.running_ = false;
        t.thread_.join();
    }
}


Fast5Pool::MapperThread::MapperThread(MapperParams &params)
    : running_(true),
      aligning_(false),
      mapper_(params) {}

Fast5Pool::MapperThread::MapperThread(MapperThread &&mt) 
    : running_(mt.running_),                                             
      aligning_(mt.aligning_),                                           
      mapper_(mt.mapper_),
      thread_(std::move(mt.thread_)) {}

void Fast5Pool::MapperThread::start() {
    thread_ = std::thread(&Fast5Pool::MapperThread::run, this);
}

void Fast5Pool::MapperThread::run() {
    std::string fast5_id;
    std::vector<float> fast5_signal;
    ReadLoc loc;
    while (running_) {
        if (signals_in_.empty()) {
            aligning_ = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        aligning_ = true;
        
        in_mtx_.lock(); 
        signals_in_.front().swap(fast5_signal);
        ids_in_.front().swap(fast5_id);
        ids_in_.pop_front();
        signals_in_.pop_front();
        in_mtx_.unlock();

        mapper_.new_read(fast5_id);
        loc = mapper_.add_samples(fast5_signal);

        out_mtx_.lock(); 
        locs_out_.push_back(loc);
        out_mtx_.unlock();
    }
}
