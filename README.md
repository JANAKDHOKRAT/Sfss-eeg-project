
# 🛠️ SFSS Core (C++ Library Build & Test)

# Before running the Rust system, you can build and test the original SFSS implementation.

# Requirements
## emp-toolkit: https://github.com/emp-toolkit/emp-tool
## CMake (>= 3.12): https://cmake.org
## GMP (tested with 6.3.0): https://gmplib.org
```
1. Build C++ SFSS
cd SFSS-main
mkdir build && cd build
cmake ..
make 
```
## Run (Standalone)
## ./bin/sfss_main
## Run (Network Demo)
# Terminal 1
````
./bin/client_runner_main
````

# Terminal 2
````
./bin/server_runner_main 0 12345
````

# Terminal 3
````
./bin/server_runner_main 1 12346
````

# 🚀 How to Run (Full Rust System)
```
1. Build C++ SFSS
cd SFSS-main
mkdir build && cd build
cmake ..
make 
```
## 2. Set environment variables
````
export SFSS_LIB_DIR=path/to/build
export SFSS_INCLUDE=path/to/SFSS-main

`````

## 3. Run services (in order)
# Terminal 1
````
cargo run -p leader
````


# Terminal 2
````
cargo run -p helper
````

# Terminal 3
```
cargo run -p reconstruct
```

# Terminal 4
````
cargo run -p agent --bin agent
````
# Terminal 4
# 4. Dashboard 
````
cd dashboard
python3 -m http.server 3000
````
## Open:

## http://127.0.0.1:3000/index2.html
## http://127.0.0.1:3000/histogram.html  

# 📷 How to Run the Webcam Tracker

# The webcam tracker sends attention scores to the Rust webcam-agent.

1. Install Python requirements FROM POWERSHELL
````
cd webcam-tracker
winget install Python.Python.3.10  
python -m venv .venv310
source .venv310/bin/activate
pip install opencv-python mediapipe numpy requests Pillow
pip install mediapipe==0.10.21 opencv-python numpy requests   
````
## 3. Run services (in order)
# Terminal 1
````
cargo run -p leader
````


# Terminal 2
````
cargo run -p helper
````

# Terminal 3
```
cargo run -p reconstruct
```

# Terminal 4
````
cargo run -p agent --bin agent
````
# Terminal 5
````
python -m venv .venv310
source .venv310/bin/activate
python webcam_attention.py --camera 0 --max-faces 4
````

# Terminal 6
# 4. Dashboard 
````
cd dashboard
python3 -m http.server 3000
````
## Open:

## http://127.0.0.1:3000/index2.html
## http://127.0.0.1:3000/histogram.html  

# 🧪 Bench Run

## The bench compares SFSS and Prio3.

## Run bench
````
cd rust-workspace/bench
cargo run -p bench 

````


🧪 Example Output
[User 123] → Leader seq=0 200 OK
[User 123] → Helper seq=0 200 OK
[User 123] seq=0 transmitted
📊 Applications
Privacy-preserving health monitoring
EEG-based attention tracking
Secure analytics pipelines
Federated data aggregation
Real-time confidential signal processing
🧑‍💻 Author

Janak Dhokrat
