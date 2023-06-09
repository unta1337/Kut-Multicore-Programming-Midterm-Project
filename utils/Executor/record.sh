#!/bin/bash

execute=$1
email=$2
key=$3
doc=$4
modes=("serial" "parallel" "cuda")
tolerances=("15.0" "30.0")
obj_files=(assets/tests/*.obj)

for mode in "${modes[@]}"; do
  for tolerance in "${tolerances[@]}"; do
    for obj_file in "${obj_files[@]}"; do
      $execute "$mode" "$tolerance" "$obj_file"
      filename=$(basename -- "$obj_file")
      filename="${filename%.*}"
      segmented_file="assets/tests/Segmented_${mode}_${tolerance}_${filename}.txt"
      node utils/LogTool/parser.js --f "$segmented_file" -e "$email" -k "$key" -d "$doc"
    done
  done
done
