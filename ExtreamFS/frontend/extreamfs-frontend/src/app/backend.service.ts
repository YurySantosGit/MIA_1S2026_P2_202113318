import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';

export interface HealthResponse {
  status: string;
}

export interface ReportItem {
  name: string;
  path: string;
}

export interface ExecuteResponse {
  success: boolean;
  output: string;
  reports: ReportItem[];
}

export interface PartitionItem {
  name: string;
  type: string;
  fit: string;
  start: number;
  size: number;
}

export interface PartitionsResponse {
  partitions: PartitionItem[];
}

export interface DiskItem {
  name: string;
  path: string;
  size: number;
}

export interface DisksResponse {
  disks: DiskItem[];
  error?: string;
}

export interface FileSystemResponse {
  success: boolean;
  path: string;
  output: string;
}

@Injectable({
  providedIn: 'root'
})
export class BackendService {
  private apiUrl = 'http://localhost:8080/api';

  constructor(private http: HttpClient) {}

  checkHealth(): Observable<HealthResponse> {
    return this.http.get<HealthResponse>(`${this.apiUrl}/health`);
  }

  executeCommands(commands: string): Observable<ExecuteResponse> {
    return this.http.post<ExecuteResponse>(`${this.apiUrl}/execute`, {
      commands
    });
  }

  getDisks(): Observable<DisksResponse> {
    return this.http.get<DisksResponse>(`${this.apiUrl}/disks`);
  }

  getPartitions(path: string): Observable<PartitionsResponse> {
    return this.http.get<PartitionsResponse>(
      `${this.apiUrl}/partitions?path=${encodeURIComponent(path)}`
    );
  }

  getFileSystem(path: string = '/'): Observable<FileSystemResponse> {
    return this.http.get<FileSystemResponse>(
      `${this.apiUrl}/fs?path=${encodeURIComponent(path)}`
    );
  }

}