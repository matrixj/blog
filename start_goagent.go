package main

import (
            "os"
            "os/exec"
            "fmt"
       )

const PATH = "/tmp/goagent.pid"

func main() {
    stat, err := os.Stat(PATH)
    checkErr(err)

}

func checkErr(err Error) {
    if err != nil {
        fmt.Print(err)
        os.Exit()
    } else {
        return
    }
}
