#!/usr/bin/env python3
import os
import re

EXCLUDES = {'.git', 'build', '.idea', 'cmake-build-debug', '__pycache__', '.pytest_cache'}

def replace_content(filepath):
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except UnicodeDecodeError:
        return

    new_content = re.sub(r'vajra', 'aham', content)
    new_content = re.sub(r'Vajra', 'Aham', new_content)
    new_content = re.sub(r'VAJRA', 'AHAM', new_content)
    
    # Also handle 'vrkah' as there are remnants in the codebase from the previous rename attempt
    new_content = re.sub(r'vrkah', 'aham', new_content)
    new_content = re.sub(r'Vrkah', 'Aham', new_content)
    new_content = re.sub(r'VRKAH', 'AHAM', new_content)

    if new_content != content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"Updated content in {filepath}")

def rename_item(item_path):
    dir_name, file_name = os.path.split(item_path)
    new_name = re.sub(r'vajra', 'aham', file_name)
    new_name = re.sub(r'Vajra', 'Aham', new_name)
    new_name = re.sub(r'VAJRA', 'AHAM', new_name)
    
    # Handle vrkah as well
    new_name = re.sub(r'vrkah', 'aham', new_name)
    new_name = re.sub(r'Vrkah', 'Aham', new_name)
    new_name = re.sub(r'VRKAH', 'AHAM', new_name)
    
    if new_name != file_name:
        new_path = os.path.join(dir_name, new_name)
        os.rename(item_path, new_path)
        print(f"Renamed {item_path} -> {new_path}")
        return new_path
    return item_path

def process_directory(root_dir):
    paths_to_process = []
    
    # 1. Collect all paths, pruning excluded folders
    for root, dirs, files in os.walk(root_dir, topdown=True):
        dirs[:] = [d for d in dirs if d not in EXCLUDES]
        paths_to_process.append((root, dirs.copy(), files.copy()))
        
    # 2. Iterate bottom-up (reverse order of collection)
    for root, dirs, files in reversed(paths_to_process):
        for file in files:
            if file == '.DS_Store' or file == 'rename_aham.py':
                continue
            filepath = os.path.join(root, file)
            # update content before renaming the file
            replace_content(filepath)
            rename_item(filepath)
            
        # rename directories
        for d in dirs:
            dirpath = os.path.join(root, d)
            rename_item(dirpath)

if __name__ == "__main__":
    process_directory(".")
