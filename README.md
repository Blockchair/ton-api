# THE API SHOULD NOT BE EXPOSED TO PUBLIC IF YOU USE [INDEXER](https://github.com/Blockchair/ton-indexer) BECAUSE SQL INJECTION CAN BE EXPLOITED

# What is it
It is a submodule for a TON blockchain project that provides JSON-RPC-like API. Using this API you can send requests to open or your TON nodes and get answer in JSON format. 


# Preparation 
This project is a submodule of TON. To start building the project you need to add in global CMakeLists file:

```add_subdirectory(blockchain-api)```

Also you need MHD and libpq to be installed.

# Building
All additional libraries are needed to be installed before we start building. This project uses same stack as TON community. So you don't need to install some special libraries except mentioned before. It also uses cmake and clang with C++-14. 
Use cmake to build it:

```cmake --build <path to build directory> --config Debug --target blockchain-api --```

# Starting 
The application has standard args:
- -h  - prints_help
- -I  - hides ips from status
- -u  - change user
- -C  - file to read global config
- -a  - connect to node ip:port
- -p  - remote public key for node
- -v  - set verbosity level
- -D  - set database config file (as it example)
- -H  - listen on http port

Simply you can start it and connect to any node from global config, in example:

``` ./blockchain-api -C ton-global.config -H 14001 ```

# Usage

After the application was built and started, you can try a simple request to API:
```bash
curl --location --request GET 'http://ip:port/lastNum'
```

And it will give you an answer:

``` json
{
    "-1": {
        "8000000000000000": {
            "seqno": 27757512,
            "roothash": "A413222C718F9B0D28BABFFFA9E6DF0C66891A26B11FB268AA3E30C7BC63E0FE",
            "filehash": "23E7A553CC7EA9567EB984CDDF5DAB2E08404890E0D5C141DC296B9724EC9716"
        }
    },
    "0": {
        "8000000000000000": {
            "seqno": 33335875,
            "roothash": "6D288D95A58B4414179F1815A78B5D96CB5FE10D3C378EB950461CAAD92F51A4",
            "filehash": "CD04B0430EEF3846CE05510E4F3A42667F90C75BFAEDA39F6D1EBBF9796E50A4"
        }
    }
}
```

# All endpoints
You can find all endpoints with examples of answers in swagger by building a docker container with it:

```docker run -e SWAGGER_JSON=ton.yaml -e SWAGGER_PORT=8080 -p 8080:8080 —name swagger —network=<ton api network name> swagger/swagger-ui```