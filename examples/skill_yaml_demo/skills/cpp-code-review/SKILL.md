---
name: cpp-code-review
description: >-
  审查 C++ 代码的内存安全、可移植性和 API 正确性。
  当用户要求审查 C++ 代码时选择此 Skill。
license: MIT
compatibility: |
  要求审查者了解 C++20。
  无需外部服务。
metadata:
  author: "myAgent examples"
  version: "1.0"
allowed-tools: 'Read'
---

# C++ 代码审查

CHECK_MEMORY_BOUNDS: 检查容器索引与内存分配大小。
如有需要，在开始审查前读取 references/review-checklist.md。
绝不自动执行脚本。
