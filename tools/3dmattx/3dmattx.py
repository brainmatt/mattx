from flask import Flask, jsonify
from flask import render_template
import re

app = Flask(__name__)

def parse_mattx_nodes():
    """Parse /proc/mattx/nodes and return structured data"""
    nodes = []
    
    try:
        with open('/proc/mattx/nodes', 'r') as f:
            lines = f.readlines()
        
        # Skip header lines until we find actual node data
        in_data_section = False
        for line in lines:
            line = line.strip()
            
            # Skip empty lines and header separators
            if not line or line.startswith('----') or line.startswith('MattX'):
                continue
            
            # Skip the "Balancer Enabled" line
            if line.startswith('Balancer'):
                continue
            
            # Skip the column header line
            if 'Node ID' in line:
                in_data_section = True
                continue
            
            # Process data lines
            if in_data_section and line:
                # Parse the node data using regex to handle multiple whitespace/tabs
                # Pattern: NodeID (optional Local), IP, CPULoad, MemFree
                match = re.match(r'(\d+)\s*(?:\(Local\))?\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+(\d+)', line)
                
                if match:
                    node_id, ip_address, cpu_load, mem_free = match.groups()
                    nodes.append({
                        "nodeid": node_id,
                        "ipaddress": ip_address,
                        "cpuload": cpu_load,
                        "memfree": mem_free
                    })
    
    except FileNotFoundError:
        # Handle case where /proc/mattx/nodes doesn't exist
        return {"error": "File /proc/mattx/nodes not found"}
    except Exception as e:
        return {"error": str(e)}
    
    return nodes

@app.route('/')
def webroot():
    return render_template('index.html')

@app.route('/mattx', methods=['GET'])
def mattx():
    """API endpoint to retrieve MattX cluster node information"""
    nodes_data = parse_mattx_nodes()
    
    if isinstance(nodes_data, dict) and "error" in nodes_data:
        return jsonify(nodes_data), 404
    
    return jsonify({"mattx": nodes_data})


if __name__ == '__main__':
    app.run(debug=True, host='0.0.0.0', port=5000)
    