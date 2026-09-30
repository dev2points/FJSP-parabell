Each instance name comprises three parts: [size group]-[homogeneity level]-[index]
- size group: encodes the basic set sizes
	- sm: 5 operations per job, 3 resources
	- md: 15 operations per job, 5 resources
	- lg: 20 operations per job, 8 resources
- homogeneity level ({0,0.5,0.75,1}): encodes the expected overlap of identical operations between any two jobs in an instance 
- index: counts instances within each group [size group]-[homogeneity level]. The number of jobs in an instance correspond to the index:
	- sm: 1-5: 2 jobs, 6-10: 3 jobs, 11-15: 4 jobs, 16-20: 5 jobs
	- md: 1-5: 10 jobs, 6-10: 20 jobs, 11-15: 30 jobs
	- sm: 1-5: 50 jobs, 6-10: 60 jobs, 11-15: 70 jobs

Each instance.json file contains the following information:
- jobs: 						list of job names
- resources: 					list of resource names
- operation_types: 				list of operation types (= setup states)
- operations: 					list of operations
- operations_jobs: 				dictionary assigning each operation to a job
- operations_operation_types: 	dictionary assigning each operation an operation type
- precedence_constraints: 		3-level dictionary, [job_1][operation_1][operation_2] = 1, if operation_1 must precede operation_2
- processing_times: 			2-level dictionary, [operation][resource] = time required for the operation on the suitable resource
- setup_times: 					3-level dictionary, [operation_type_1][operation_type_2][resource] = setup time required for the transition from operation_type_1 to operation_type_2 on resource that can achieve both setup states
- transport_times: 				2-level dictionary, [resource_1][resource_2] = time required for the transport from resource_1 to resource_2
- blocked_times: 				dictionary assigning a release date (blocked time from the beginning of the planning horizon) to each resource 
- operation_classes: 			dictionary designating each operation as an assembly operation (if value=1) or component operation (if value=0)
- resources_for_k: 				dictionary listing the suitable resources for each operation type / resources that can reach each setup state