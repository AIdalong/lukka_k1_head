#!/usr/bin/env python3
"""
检查 assets 目录中的文件列表变化
如果发现新文件或文件列表变化，自动触发 CMake 重新配置
参考 emoji 资源的处理方式
"""

import os
import sys
import glob
import hashlib

def get_file_list_hash(dirs):
    """获取文件列表的哈希值"""
    all_files = []
    for dir_path in dirs:
        if os.path.exists(dir_path):
            files = glob.glob(os.path.join(dir_path, "*.p3"))
            all_files.extend(sorted(files))
    # 计算文件列表的哈希值
    file_list_str = "\n".join(all_files)
    return hashlib.md5(file_list_str.encode()).hexdigest()

def main():
    if len(sys.argv) < 4:
        print("Usage: check_assets.py <lang_dir> <common_dir> <stamp_file>")
        sys.exit(1)
    
    lang_dir = sys.argv[1]
    common_dir = sys.argv[2]
    stamp_file = sys.argv[3]
    
    # 获取当前文件列表的哈希值
    current_hash = get_file_list_hash([lang_dir, common_dir])
    
    # 读取之前的哈希值
    old_hash = None
    if os.path.exists(stamp_file):
        with open(stamp_file, 'r') as f:
            old_hash = f.read().strip()
    
    # 如果哈希值不同，说明文件列表变化了
    if old_hash != current_hash:
        print(f"Asset files changed (old: {old_hash[:8] if old_hash else 'none'}, new: {current_hash[:8]})")
        print("New or modified asset files detected, triggering CMake reconfigure...")
        
        # 更新 stamp 文件
        with open(stamp_file, 'w') as f:
            f.write(current_hash)
        
        # 更新 CMakeCache.txt 的时间戳，触发重新配置
        cmake_cache = os.path.join(os.path.dirname(stamp_file), "CMakeCache.txt")
        if os.path.exists(cmake_cache):
            os.utime(cmake_cache, None)
            print("CMakeCache.txt updated, reconfigure will happen on next build")
        else:
            print("CMakeCache.txt not found, please run 'idf.py reconfigure' manually")
    else:
        print("Asset files unchanged")
    
    # 保存当前哈希值
    with open(stamp_file, 'w') as f:
        f.write(current_hash)

if __name__ == "__main__":
    main()

