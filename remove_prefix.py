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

    # Replace prefixes explicitly
    new_content = re.sub(r'aham_', '', content)
    new_content = re.sub(r'Aham_', '', new_content)
    new_content = re.sub(r'AHAM_', '', new_content)

    if new_content != content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"Updated content in {filepath}")

def rename_item(item_path):
    dir_name, file_name = os.path.split(item_path)
    new_name = re.sub(r'aham_', '', file_name)
    new_name = re.sub(r'Aham_', '', new_name)
    new_name = re.sub(r'AHAM_', '', new_name)
    
    if new_name != file_name:
        new_path = os.path.join(dir_name, new_name)
        os.rename(item_path, new_path)
        print(f"Renamed {item_path} -> {new_path}")
        return new_path
    return item_path

def process_directory(root_dir):
    paths_to_process = []
    
    for root, dirs, files in os.walk(root_dir, topdown=True):
        dirs[:] = [d for d in dirs if d not in EXCLUDES]
        paths_to_process.append((root, dirs.copy(), files.copy()))
        
    for root, dirs, files in reversed(paths_to_process):
        for file in files:
            if file == '.DS_Store' or file == 'remove_prefix.py':
                continue
            filepath = os.path.join(root, file)
            replace_content(filepath)
            rename_item(filepath)
            
        for d in dirs:
            dirpath = os.path.join(root, d)
            rename_item(dirpath)

if __name__ == "__main__":
    process_directory(".")
