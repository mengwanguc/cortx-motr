//motr datastore
package mds

import (
//    "os"
    "fmt"
//    "flag"
    "log"
    "strings"
    "motr/mio"
    ds "github.com/ipfs/go-datastore"
    dsq "github.com/ipfs/go-datastore/query"    
)


type MotrDS struct {
    Config     mio.Config
    IndexID    string
    Mkv        *mio.Mkv
}

func Open(conf mio.Config, indexID string) (*MotrDS, error) {
    mio.InitWithConfig(conf)
    var mkv mio.Mkv
    createFlag := true
    if err := mkv.Open(indexID, createFlag); err != nil {
        log.Fatalf("failed to open index %v: %v", indexID, err)
        return nil, err
    }
//    defer mkv.Close()


    return &MotrDS {
        Config:    conf,
        IndexID:   indexID,
        Mkv:       &mkv,
    }, nil
}


func (mds *MotrDS) Put(k ds.Key, value []byte) error {
    err := mds.Mkv.Put([]byte(k.String()), value, true)
    return err
}

func (mds *MotrDS) Get(k ds.Key) ([]byte, error) {
    val, err := mds.Mkv.Get([]byte(k.String()))
    if len(val) == 0 {
        if strings.HasSuffix(err.Error(), "-2") == true {
            return val, ds.ErrNotFound
        }
    }
    return val, err
}


func (mds *MotrDS) GetSize(k ds.Key) (int, error) {
    val, err := mds.Mkv.Get([]byte(k.String()))
    if err != nil {
        if strings.HasSuffix(err.Error(), "-2") == true {
            if len(val) == 0 {
                return -1, ds.ErrNotFound
            }
        }
        return -1, err
    }
    return len(val), nil
}

func (mds *MotrDS) Delete(k ds.Key) error {
    err :=  mds.Mkv.Delete([]byte(k.String()))
    return err
}




func (mds *MotrDS) Has(k ds.Key) (bool, error) {
    val, err :=  mds.Mkv.Get([]byte(k.String()))
    if len(val) == 0  {
        if strings.HasSuffix(err.Error(), "-2") == true {
            return false, nil
        }
        return false, err
    }
    return true, err
}


func (mds *MotrDS) Sync(prefix ds.Key) error {
	return nil
}

func (mds *MotrDS) Query(q dsq.Query) (dsq.Results, error) {
    return nil, nil
}

func (mds *MotrDS) Close() error {
    mds.Mkv.Close()
    return nil
}
