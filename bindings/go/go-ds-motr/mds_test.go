package mds


import (
    "bytes"
    "testing"
    "motr/mio"
    ds "github.com/ipfs/go-datastore"

    dstest "github.com/ipfs/go-datastore/test"
)

func TestSuite(t *testing.T) {
    config := mio.Config{
        LocalEP:    "10.52.3.172@tcp:12345:33:1000",
        HaxEP:      "10.52.3.172@tcp:12345:34:1",
        Profile:    "0x7000000000000001:0",
        ProcFid:    "0x7200000000000001:64",
        TraceOn:    false,
        Verbose:    false,
        ThreadsN:   1,
    }
    indexID := "0x7800000000000001:123456701"
    motrds, err := Open(config, indexID)

    k := ds.NewKey("foo")
    val := []byte("Hello Datastore!")

    err = motrds.Put(k, val)
    if err != nil {
        t.Fatal("error putting foo to datastore: ", err)
    }


    getval,err := motrds.Get(k)

    if err != nil {
        t.Fatal("errr getting value of key foo from datastore: ", err)
    }

    if !bytes.Equal(getval, val) {
        t.Fatal("value received on get for key foo wasnt what we expected:", string(getval))
    }


    have, err := motrds.Has(k)
    if err != nil {
        t.Fatal("error calling has on key foo ", err)
    }

    if !have {
        t.Fatal("should have key foo, has returned false")
    }

    size, err := motrds.GetSize(k)
    if err != nil {
        t.Fatal("error calling GetSize on key foo ", err)
    }

    if size != len(val) {
        t.Fatal("should have size ", len(val) , "GetSize returned ", size)
    }






    newk := ds.NewKey("ddd")

    have, err = motrds.Has(newk)
    if err != nil {
        t.Fatal("error calling has on key ddd ", err)
    }

    if have {
        t.Fatal("shouldn't have key ddd, has returned true")
    }


    err = motrds.Delete(k)
    if err != nil {
        t.Fatal("error calling delete on key foo ", err)
    }

    have, err = motrds.Has(k)
    if err != nil {
        t.Fatal("error calling has on key foo after delete", err)
    }

    if have {
        t.Fatal("should have deleted key foo, has returned true")
    }

    // test from go-datastore
    t.Run("basic operations", func(t *testing.T) {
        dstest.SubtestBasicPutGet(t, motrds)
    })


    motrds.Mkv.Close()

}
