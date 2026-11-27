//go:build !windows

package ml

func QcomNpuBackendPath() string { return "" }
func QcomNpuDefaults()           {}
