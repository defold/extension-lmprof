import json
import sys

# Get the filename from the command-line arguments
filename = sys.argv[1]

# Load the JSON file
with open(filename) as f:
    data = json.load(f)

# Create a dictionary to store the count of each field name
count_dict = {}
begin_dict = {}
time_dict = {}

# Loop through each object in the JSON file
for obj in data:
    # Check if the "ph" field is "B"
    if obj.get('ph') == 'B':
        # Get the name of the field
        field_name = obj['name']
        begin_dict[field_name] = obj['ts']
        # If the field name is not already in the dictionary, add it with a count of 1
        if field_name not in count_dict:
            count_dict[field_name] = 1
        # If the field name is already in the dictionary, increment its count
        else:
            count_dict[field_name] += 1
    if obj.get('ph') == 'E':
        field_name = obj['name']
        if field_name in begin_dict:
            if field_name not in time_dict:
                time_dict[field_name] = obj['ts'] - begin_dict[field_name]
            else:
                time_dict[field_name] = time_dict[field_name] + (obj['ts'] - begin_dict[field_name])

# Sort the dictionary by count in descending order
sorted_dict = {k: v for k, v in sorted(count_dict.items(), key=lambda item: item[1], reverse=True)}

with open('lprof_stat.csv','w') as file:
    # Print each field count, tab, and name on its own line
    for field_name, count in sorted_dict.items():
        print(f"{count}\t{field_name}\t{format(time_dict[field_name] * 0.001, '.2f')}\tms")
        
        file.write(f"{count},{field_name.replace(',', ' comma')},{format(time_dict[field_name] * 0.001, '.2f')}\n")
