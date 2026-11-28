@echo off
set "GGML_BACKEND_PATH=%~dp0ggml-hexagon.dll"
set "GGML_NPU_REQUIRE_FULL_OFFLOAD=1"
set "GGML_HEXAGON_MBUF=512,256"
set "MODEL_DIR=%~dp0models"
set COMMANDLINE_ARGS=-m "%~dp0models\checkpoints\SDXS-512-Q8_0.gguf" --backend HTP0 --params-backend HTP0 --auto-fit off --steps 1 --cfg-scale 1 -W 512 -H 512 --flash-attn on --conv-direct on
