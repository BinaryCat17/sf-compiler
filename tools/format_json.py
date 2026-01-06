import json
import os
import glob

def node_sort_key(node):
    """
    Sort order:
    0. Const
    1. Input
    2. Output
    3. Index (System Inputs)
    4. Everything else (Logic, Math, etc.)
    Secondary sort: by 'id'
    """
    n_type = node.get("type", "")
    if n_type == "Const":
        priority = 0
    elif n_type == "Input":
        priority = 1
    elif n_type == "Output":
        priority = 2
    elif n_type == "Index":
        priority = 3
    else:
        priority = 4
        
    return (priority, node.get("id", ""))

def format_json_sionflow(filepath):
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
        
        # Sort nodes if present
        if "nodes" in data and isinstance(data["nodes"], list):
            data["nodes"].sort(key=node_sort_key)

        output = "{\n"
        keys = list(data.keys())
        
        for k_idx, key in enumerate(keys):
            val = data[key]
            output += f'    "{key}": '
            
            if key in ["nodes", "links"] and isinstance(val, list):
                output += "[\n"
                
                prev_priority = -1
                
                for i, item in enumerate(val):
                    # Check for group change to insert spacing (only for nodes)
                    if key == "nodes":
                        priority, _ = node_sort_key(item)
                        if prev_priority != -1 and priority != prev_priority:
                            output += "\n"
                        prev_priority = priority

                    # SECURE one-line format using separators
                    line = json.dumps(item, ensure_ascii=False, separators=(', ', ': '))
                    # Add padding inside root braces
                    if line.startswith('{') and line.endswith('}'):
                        line = '{ ' + line[1:-1] + ' }'
                    
                    output += f'        {line}'
                    if i < len(val) - 1:
                        output += ","
                    output += "\n"
                output += "    ]"
            else:
                val_str = json.dumps(val, indent=4, ensure_ascii=False)
                indented = val_str.replace('\n', '\n    ')
                output += indented
                
            if k_idx < len(keys) - 1:
                output += ","
            output += "\n"
            
        output += "}"
        
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(output)
            f.write('\n')
            
        print(f"Formatted {filepath}")
    except Exception as e:
        print(f"Error processing {filepath}: {e}")

if __name__ == "__main__":
    files = []
    for pattern in ['**/*.json', '**/*.mfapp']:
        files.extend(glob.glob(pattern, recursive=True))
    
    # Exclude system files and build directories
    exclude_list = ['build/', 'out/', 'vcpkg', '.git/', 'CMakePresets.json', 'vcpkg.json']
    files = [f for f in files if not any(x in f for x in exclude_list)]
    
    for f in files:
        format_json_sionflow(f)