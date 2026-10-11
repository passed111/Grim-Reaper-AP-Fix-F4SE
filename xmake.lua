add_rules("mode.releasedbg")

target("GrimReaperAPFix")
    add_rules("commonlibf4.plugin", {
		author = "SOLO",
		description = "Fixes Grim Reaper's Sprint restoring AP only to the permanent cap, ignoring temporary bonuses"
    })
    add_deps("commonlibf4")
    add_files("src/**.cpp")
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
