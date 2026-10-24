# Public WoWReMac configuration names. Keep older cache variables as hidden aliases so
# existing build directories and branch-specific tooling do not silently lose settings.
function(wowremac_cache_setting name legacy type help default_value)
    if (NOT DEFINED ${name} AND DEFINED ${legacy})
        set(${name} "${${legacy}}" CACHE ${type} "${help}")
    elseif (DEFINED ${legacy} AND DEFINED _${name}_PREVIOUS
            AND "${${name}}" STREQUAL "${_${name}_PREVIOUS}"
            AND NOT "${${legacy}}" STREQUAL "${_${name}_PREVIOUS}")
        # An old script changed only the compatibility alias in an existing build tree.
        set(${name} "${${legacy}}" CACHE ${type} "${help}" FORCE)
    else ()
        set(${name} "${default_value}" CACHE ${type} "${help}")
    endif ()

    set(_${name}_PREVIOUS "${${name}}" CACHE INTERNAL "Previous ${name} setting" FORCE)
    set(${legacy} "${${name}}" CACHE ${type} "Compatibility alias for ${name}" FORCE)
    mark_as_advanced(${legacy})
endfunction()
