//go:build windows

package ml

import (
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows/registry"
)

// QcomNpuBackendPath resolves the signed backend from the installed driver,
// independently of the process's inherited environment or working directory.
func QcomNpuBackendPath() string {
	key, err := registry.OpenKey(registry.LOCAL_MACHINE, `SYSTEM\CurrentControlSet\Services\qcom_npu`, registry.QUERY_VALUE)
	if err != nil {
		return ""
	}
	defer key.Close()
	image, _, err := key.GetStringValue("ImagePath")
	if err != nil {
		return ""
	}
	if strings.HasPrefix(strings.ToLower(image), `\systemroot\`) {
		image = filepath.Join(os.Getenv("SystemRoot"), image[len(`\SystemRoot\`):])
	}
	image = strings.TrimPrefix(image, `\??\`)
	if !filepath.IsAbs(image) {
		return ""
	}
	backend := filepath.Join(filepath.Dir(image), "GGML", "qcom_npu_ggml.dll")
	info, err := os.Stat(backend)
	if err != nil || !info.Mode().IsRegular() {
		return ""
	}
	return backend
}

// Keep each model's queue serial while allowing independent model processes.
// Explicit configuration takes precedence over these defaults.
func QcomNpuDefaults() {
	backend := QcomNpuBackendPath()
	if backend == "" {
		return
	}
	inherited := os.Getenv("GGML_BACKEND_PATH")
	if inherited == "" || (strings.Contains(strings.ToLower(inherited), `\driverstore\filerepository\qcom_npu.inf_`) &&
		strings.EqualFold(filepath.Base(inherited), "qcom_npu_ggml.dll")) {
		_ = os.Setenv("GGML_BACKEND_PATH", backend)
	}
	for key, value := range map[string]string{
		"OLLAMA_MAX_LOADED_MODELS":      "2",
		"OLLAMA_NUM_PARALLEL":           "1",
		"OLLAMA_CONTEXT_LENGTH":         "2048",
		"GGML_NPU_REQUIRE_FULL_OFFLOAD": "1",
	} {
		if _, set := os.LookupEnv(key); !set {
			_ = os.Setenv(key, value)
		}
	}
}
