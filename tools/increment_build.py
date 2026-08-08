#!/usr/bin/env python3
"""
Pre-build script to increment build number and generate version.h
Handles semantic versioning: MAJOR.MINOR.BUILD
- If MAJOR or MINOR changed: resets BUILD_NUMBER to 0
- Otherwise: increments BUILD_NUMBER
"""

import os
import re
import time
from pathlib import Path

# PROJECT_ROOT is one level up from tools/  -> Repo-Root
# Der Build-Zaehler (.build_number/.last_version) ist GEMEINSAM fuer alle Projekte
# im Repo (esp32cam/ und cyd/ teilen sich die globale v0.1.xx-Nummerierung).
# Die include/-Pfade werden pro Projekt ueber --project-dir gesetzt.
PROJECT_ROOT = Path(__file__).parent.parent
BUILD_NUMBER_FILE = PROJECT_ROOT / ".build_number"
LAST_VERSION_FILE = PROJECT_ROOT / ".last_version"

def read_version_from_config(config_header):
    """Extract APP_VERSION_MAJOR and APP_VERSION_MINOR from config.h"""
    major, minor = None, None
    
    with open(config_header, 'r') as f:
        content = f.read()
    
    major_match = re.search(r'#define\s+APP_VERSION_MAJOR\s+(\d+)', content)
    minor_match = re.search(r'#define\s+APP_VERSION_MINOR\s+(\d+)', content)
    
    if major_match:
        major = int(major_match.group(1))
    if minor_match:
        minor = int(minor_match.group(1))
    
    return major, minor

def get_last_version():
    """Read the last saved MAJOR.MINOR from .last_version file"""
    if LAST_VERSION_FILE.exists():
        with open(LAST_VERSION_FILE, 'r') as f:
            line = f.read().strip()
            parts = line.split('.')
            if len(parts) >= 2:
                return int(parts[0]), int(parts[1])
    return None, None

def save_version(major, minor):
    """Save current MAJOR.MINOR to .last_version file"""
    with open(LAST_VERSION_FILE, 'w') as f:
        f.write(f"{major}.{minor}")

def increment_build_number(major, minor):
    """
    Increment build number, or reset if MAJOR/MINOR changed
    Returns (build_num, was_reset)
    """
    last_major, last_minor = get_last_version()
    
    # Check if MAJOR or MINOR changed
    if last_major is not None and (major != last_major or minor != last_minor):
        # Version changed: reset BUILD_NUMBER to 1 (beginnend bei 1)
        build_num = 1
        was_reset = True
    else:
        # Same version: increment BUILD_NUMBER
        if BUILD_NUMBER_FILE.exists():
            with open(BUILD_NUMBER_FILE, 'r') as f:
                build_num = int(f.read().strip())
        else:
            build_num = 0
        build_num += 1
        was_reset = False
    
    # Save new build number and version
    with open(BUILD_NUMBER_FILE, 'w') as f:
        f.write(str(build_num))
    
    save_version(major, minor)
    
    return build_num, was_reset

def generate_version_header(major, minor, build_num, version_template, version_header):
    """Generate version.h from version.h.in template"""
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    
    with open(version_template, 'r') as f:
        content = f.read()
    
    # Replace template variables
    content = content.replace("@VERSION_MAJOR@", str(major))
    content = content.replace("@VERSION_MINOR@", str(minor))
    content = content.replace("@BUILD_NUMBER@", str(build_num))
    content = content.replace("@BUILD_TIMESTAMP@", timestamp)
    
    # Generate full version string: v0.1.4
    version_string = f"v{major}.{minor}.{build_num}"
    content = content.replace("@VERSION_STRING@", version_string)
    
    with open(version_header, 'w') as f:
        f.write(content)
    
    return version_string

if __name__ == "__main__":
    try:
        import argparse
        parser = argparse.ArgumentParser(description="Build-Nummer erhoehen und version.h generieren")
        parser.add_argument("--project-dir", default=str(PROJECT_ROOT),
                            help="Projektverzeichnis mit include/ (config.h, version.h.in)")
        args = parser.parse_args()
        project = Path(args.project_dir).resolve()
        config_header = project / "include" / "config.h"
        version_template = project / "include" / "version.h.in"
        version_header = project / "include" / "version.h"

        # Read MAJOR and MINOR from config.h
        major, minor = read_version_from_config(config_header)
        if major is None or minor is None:
            raise ValueError("Could not find APP_VERSION_MAJOR or APP_VERSION_MINOR in config.h")
        
        # Increment build number (or reset if version changed)
        build_num, was_reset = increment_build_number(major, minor)
        
        # Generate version.h
        version_string = generate_version_header(major, minor, build_num, version_template, version_header)
        
        # Log message
        if was_reset:
            print(f"Version updated: {version_string} (BUILD_NUMBER reset to 1)")
        else:
            print(f"Build #{build_num} generated: {version_string}")
            
    except Exception as e:
        print(f"Error: {e}")
        exit(1)
